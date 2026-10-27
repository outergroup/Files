#ifndef FILE_LIST_H
#define FILE_LIST_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    const char *name, *path;
    char mode[11];
    bool directory;
    uint32_t access;
    uint64_t size, modified_ms;
} FileListEntry;
typedef void (*FileListReadEntry)(const void *context, size_t index, FileListEntry *entry);
static void file_list_u32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
static void file_list_u64(unsigned char *p, uint64_t n) {
    for (unsigned i = 0; i < 8; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
static void file_list_string(unsigned char *bytes, size_t ref, size_t *cursor, const char *value) {
    size_t n = strlen(value);
    file_list_u32(bytes + ref, (uint32_t)*cursor);
    file_list_u32(bytes + ref + 4, (uint32_t)n);
    memcpy(bytes + *cursor, value, n);
    *cursor += n;
}
static unsigned char *file_list_encode(const char *path, const char *parent, size_t count,
                                       const void *context, FileListReadEntry read_entry, size_t *length) {
    *length = 0;
    if (count > (UINT32_MAX - 48) / 48)
        return NULL;
    size_t variable = 48 + count * 48;
    uint64_t total = variable + (uint64_t)strlen(path) + strlen(parent);
    for (size_t i = 0; i < count; i++) {
        FileListEntry e = {0};
        read_entry(context, i, &e);
        total += strlen(e.name) + (uint64_t)strlen(e.path) + strlen(e.mode);
        if (total > UINT32_MAX)
            return NULL;
    }
    if (total > UINT32_MAX || total > SIZE_MAX)
        return NULL;
    unsigned char *bytes = calloc(1, (size_t)total);
    if (!bytes)
        return NULL;
    file_list_u32(bytes, 0x534c4646);
    file_list_u32(bytes + 4, 2);
    file_list_u32(bytes + 8, (uint32_t)count);
    file_list_u32(bytes + 12, 48);
    file_list_u32(bytes + 16, 48);
    file_list_u32(bytes + 20, (uint32_t)variable);
    file_list_u32(bytes + 24, (uint32_t)total);
    size_t cursor = variable;
    file_list_string(bytes, 32, &cursor, path);
    file_list_string(bytes, 40, &cursor, parent);
    for (size_t i = 0; i < count; i++) {
        FileListEntry e = {0};
        read_entry(context, i, &e);
        size_t row = 48 + i * 48;
        file_list_string(bytes, row, &cursor, e.name);
        file_list_string(bytes, row + 8, &cursor, e.path);
        file_list_string(bytes, row + 16, &cursor, e.mode);
        file_list_u32(bytes + row + 24, e.directory ? 1 : 0);
        file_list_u32(bytes + row + 28, e.access);
        file_list_u64(bytes + row + 32, e.size);
        file_list_u64(bytes + row + 40, e.modified_ms);
    }
    *length = (size_t)total;
    return bytes;
}
#endif
