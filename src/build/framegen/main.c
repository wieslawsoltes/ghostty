#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define SEPARATOR '\x01'
#define CHUNK_SIZE 16384

#ifdef _WIN32
static int compare_frame_names_qsort(const void *a, const void *b) {
    const char *const *left = (const char *const *)a;
    const char *const *right = (const char *const *)b;
    return strcmp(*left, *right);
}

static int list_frame_files(const char *frames_dir, char ***out_names) {
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*.txt", frames_dir);

    WIN32_FIND_DATAA find_data;
    HANDLE handle = FindFirstFileA(pattern, &find_data);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    size_t capacity = 64;
    size_t count = 0;
    char **names = (char **)malloc(capacity * sizeof(char *));
    if (!names) {
        FindClose(handle);
        return -1;
    }

    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }

        size_t len = strlen(find_data.cFileName);
        char *name = (char *)malloc(len + 1);
        if (!name) {
            FindClose(handle);
            return -1;
        }
        memcpy(name, find_data.cFileName, len + 1);

        if (count == capacity) {
            size_t next_capacity = capacity * 2;
            char **next = (char **)realloc(names, next_capacity * sizeof(char *));
            if (!next) {
                free(name);
                FindClose(handle);
                return -1;
            }
            names = next;
            capacity = next_capacity;
        }

        names[count++] = name;
    } while (FindNextFileA(handle, &find_data) != 0);

    FindClose(handle);
    qsort(names, count, sizeof(char *), compare_frame_names_qsort);
    *out_names = names;
    return (int)count;
}
#endif

#ifndef _WIN32
static int filter_frames(const struct dirent *entry) {
    const char *name = entry->d_name;
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".txt") == 0;
}

static int compare_frames(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}
#endif

static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size);
    if (!buf) {
        return NULL;
    }

    if (fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "Failed to read %s\n", path);
        return NULL;
    }

    fclose(f);
    *out_size = size;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <frames_dir> <output_file>\n", argv[0]);
        return 1;
    }

    const char *frames_dir = argv[1];
    const char *output_file = argv[2];

#ifdef _WIN32
    char **frame_names = NULL;
    int n = list_frame_files(frames_dir, &frame_names);
    if (n < 0) {
        fprintf(stderr, "Failed to allocate frame filename list\n");
        return 1;
    }
#else
    struct dirent **namelist;
    int n = scandir(frames_dir, &namelist, filter_frames, compare_frames);
    if (n < 0) {
        fprintf(stderr, "Failed to scan directory %s: %s\n", frames_dir, strerror(errno));
        return 1;
    }
#endif

    if (n == 0) {
        fprintf(stderr, "No frame files found in %s\n", frames_dir);
        return 1;
    }

    size_t total_size = 0;
    char **frame_contents = calloc(n, sizeof(char*));
    size_t *frame_sizes = calloc(n, sizeof(size_t));

    for (int i = 0; i < n; i++) {
        char path[4096];
#ifdef _WIN32
        snprintf(path, sizeof(path), "%s\\%s", frames_dir, frame_names[i]);
#else
        snprintf(path, sizeof(path), "%s/%s", frames_dir, namelist[i]->d_name);
#endif

        frame_contents[i] = read_file(path, &frame_sizes[i]);
        if (!frame_contents[i]) {
            return 1;
        }

        total_size += frame_sizes[i];
        if (i < n - 1) total_size++;
    }

    char *joined = malloc(total_size);
    if (!joined) {
        fprintf(stderr, "Failed to allocate joined buffer\n");
        return 1;
    }

    size_t offset = 0;
    for (int i = 0; i < n; i++) {
        memcpy(joined + offset, frame_contents[i], frame_sizes[i]);
        offset += frame_sizes[i];
        if (i < n - 1) {
            joined[offset++] = SEPARATOR;
        }
    }

    uLongf compressed_size = compressBound(total_size);
    unsigned char *compressed = malloc(compressed_size);
    if (!compressed) {
        fprintf(stderr, "Failed to allocate compression buffer\n");
        return 1;
    }

    z_stream stream = {0};
    stream.next_in = (unsigned char*)joined;
    stream.avail_in = total_size;
    stream.next_out = compressed;
    stream.avail_out = compressed_size;

    // Use -MAX_WBITS for raw DEFLATE (no zlib wrapper)
    int ret = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        fprintf(stderr, "deflateInit2 failed: %d\n", ret);
        return 1;
    }

    ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        fprintf(stderr, "deflate failed: %d\n", ret);
        deflateEnd(&stream);
        return 1;
    }

    compressed_size = stream.total_out;
    deflateEnd(&stream);
    
    FILE *out = fopen(output_file, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create %s: %s\n", output_file, strerror(errno));
        return 1;
    }

    if (fwrite(compressed, 1, compressed_size, out) != compressed_size) {
        fprintf(stderr, "Failed to write compressed data\n");
        return 1;
    }

    fclose(out);

#ifdef _WIN32
    for (int i = 0; i < n; i++) {
        free(frame_names[i]);
    }
    free(frame_names);
#else
    for (int i = 0; i < n; i++) {
        free(namelist[i]);
    }
    free(namelist);
#endif

    for (int i = 0; i < n; i++) {
        free(frame_contents[i]);
    }
    free(frame_contents);
    free(frame_sizes);
    free(joined);
    free(compressed);

    return 0;
}
