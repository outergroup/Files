#include "helper.h"
static void parent_path(const char *path, char out[PATH_MAX]) {
    snprintf(out, PATH_MAX, "%s", path);
    char *p = strrchr(out, '/');
    if (p == out)
        out[1] = 0;
    else if (p)
        *p = 0;
}
typedef struct {
    char *name, *path;
    char mode[11];
    bool directory;
    uint32_t access;
    uint64_t size, modified;
} FileEntry;
static int compare_entries(const void *a, const void *b) {
    const FileEntry *x = a, *y = b;
    if (x->directory != y->directory)
        return x->directory ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static BinaryReply list_files(const char *path) {
    BinaryReply result = {0};
    DIR *dir = opendir(path);
    if (!dir) {
        fail("%s: %s", path, strerror(errno));
        return result;
    }
    FileEntry *rows = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *full = format("%s%s%s", path, !strcmp(path, "/") ? "" : "/", entry->d_name);
        struct stat st;
        if (stat(full, &st) && lstat(full, &st)) {
            free(full);
            continue;
        }
        if (count >= (MAX_DATA - RESPONSE_SIZE) / FILE_ROW_SIZE) {
            free(full);
            fail("too many directory entries");
            break;
        }
        FileEntry *next = realloc(rows, (count + 1) * sizeof(*rows));
        if (!next) {
            free(full);
            fail("out of memory");
            break;
        }
        rows = next;
        FileEntry *r = &rows[count++];
        *r = (FileEntry){.name = strdup(entry->d_name),
                         .path = full,
                         .directory = S_ISDIR(st.st_mode),
                         .size = (uint64_t)st.st_size,
                         .access = (access(full, R_OK) == 0 ? 1 : 0) | (access(full, W_OK) == 0 ? 2 : 0)};
#ifdef __APPLE__
        r->modified = (uint64_t)st.st_mtimespec.tv_sec * 1000 + (uint64_t)st.st_mtimespec.tv_nsec / 1000000;
#else
        r->modified = (uint64_t)st.st_mtim.tv_sec * 1000 + (uint64_t)st.st_mtim.tv_nsec / 1000000;
#endif
        strcpy(r->mode, "----------");
        if (S_ISDIR(st.st_mode))
            r->mode[0] = 'd';
        else if (S_ISLNK(st.st_mode))
            r->mode[0] = 'l';
        for (int i = 0; i < 9; i++)
            if (st.st_mode & (1 << (8 - i)))
                r->mode[i + 1] = "rwx"[i % 3];
    }
    closedir(dir);
    if (!failure[0]) {
        if (count > 1)
            qsort(rows, count, sizeof(*rows), compare_entries);
        result = response_create(OP_LIST, (uint32_t)count, FILE_ROW_SIZE);
        char parent[PATH_MAX];
        parent_path(path, parent);
        binary_string(&result.metadata, 48, path);
        binary_string(&result.metadata, 56, parent);
        for (size_t i = 0; i < count && !failure[0]; i++) {
            FileEntry *r = &rows[i];
            size_t offset = RESPONSE_SIZE + i * FILE_ROW_SIZE;
            binary_string(&result.metadata, offset, r->name);
            binary_string(&result.metadata, offset + 8, r->path);
            binary_string(&result.metadata, offset + 16, r->mode);
            put_u32(result.metadata.data + offset + 24, r->directory);
            put_u32(result.metadata.data + offset + 28, r->access);
            put_u64(result.metadata.data + offset + 32, r->size);
            put_u64(result.metadata.data + offset + 40, r->modified);
        }
    }
    for (size_t i = 0; i < count; i++) {
        free(rows[i].name);
        free(rows[i].path);
    }
    free(rows);
    return result;
}
BinaryReply local_binary_operation(const Request *request, Bytes body) {
    BinaryReply r = {0};
    uint32_t op = request->operation;
    if (body.size) {
        fail("unexpected request body");
        return r;
    }
    char path[PATH_MAX];
    if (!expand_path(request->path, path))
        return r;
    switch (op) {
    case OP_LIST:
        r = list_files(path);
        break;
    case OP_READ:
        if (!request->limit || request->limit > MAX_DATA) {
            fail("invalid file size limit");
            break;
        }
        r.body = read_file(path, (size_t)request->limit, true);
        if (r.body.data) {
            Bytes data = r.body;
            r = response_create(op, 0, 0);
            r.body = data;
            binary_string(&r.metadata, 48, path);
        }
        break;
    default:
        fail("unsupported helper operation");
        break;
    }
    return r;
}
