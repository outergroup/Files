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

static void place_add(FileEntry **rows, size_t *count, const char *path, const char *name) {
    char resolved[PATH_MAX];
    struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st) || !S_ISDIR(st.st_mode)) return;
    for (size_t i = 0; i < *count; i++) {
        if (!strcmp((*rows)[i].path, resolved)) {
            if (name && *name) { free((*rows)[i].name); (*rows)[i].name = strdup(name); }
            return;
        }
    }
    if (*count >= 256) return;
    FileEntry *next = realloc(*rows, (*count + 1) * sizeof(**rows));
    if (!next) { fail("out of memory"); return; }
    *rows = next;
    const char *base = strrchr(resolved, '/');
    next[(*count)++] = (FileEntry){.path = strdup(resolved),
        .name = strdup(name && *name ? name : base && base[1] ? base + 1 : resolved)};
}
#ifdef __linux__
static void mount_path_decode(char *path) {
    char *out = path;
    for (char *p = path; *p;) {
        if (*p == '\\' && p[1] >= '0' && p[1] <= '7' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
            *out++ = (char)((p[1]-'0')*64 + (p[2]-'0')*8 + p[3]-'0'); p += 4;
        } else *out++ = *p++;
    }
    *out = 0;
}
#endif
static BinaryReply list_places(void) {
    FileEntry *rows = NULL;
    size_t count = 0;
    char home[PATH_MAX];
    if (expand_path("~", home)) place_add(&rows, &count, home, "Home");
#ifdef __linux__
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (mounts) {
        char *line = NULL; size_t capacity = 0;
        while (getline(&line, &capacity, mounts) >= 0) {
            char *save = NULL, *field = strtok_r(line, " ", &save);
            for (int i = 0; i < 4 && field; i++) field = strtok_r(NULL, " ", &save);
            if (!field) continue;
            mount_path_decode(field);
            if (!strcmp(field, "/") || !strcmp(field, "/proc") || !strncmp(field, "/proc/", 6) ||
                !strcmp(field, "/sys") || !strncmp(field, "/sys/", 5) ||
                !strcmp(field, "/dev") || !strncmp(field, "/dev/", 5) ||
                !strcmp(field, "/var/lib/outershell") || !strncmp(field, "/var/lib/outershell/", sizeof("/var/lib/outershell/") - 1)) continue;
            place_add(&rows, &count, field, NULL);
        }
        free(line); fclose(mounts);
    }
#endif
    FILE *config = fopen("/etc/outer-files/places.tsv", "r");
    if (config) {
        char *line = NULL; size_t capacity = 0;
        while (getline(&line, &capacity, config) >= 0) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line != '/') continue;
            char *label = strchr(line, '\t');
            if (!label) continue;
            *label++ = 0;
            place_add(&rows, &count, line, label);
        }
        free(line); fclose(config);
    }
    BinaryReply reply = response_create(OP_PLACES, (uint32_t)count, FILE_ROW_SIZE);
    for (size_t i = 0; i < count; i++) {
        size_t offset = RESPONSE_SIZE + i * FILE_ROW_SIZE;
        binary_string(&reply.metadata, offset, rows[i].name);
        binary_string(&reply.metadata, offset + 8, rows[i].path);
        binary_string(&reply.metadata, offset + 16, "");
        put_u32(reply.metadata.data + offset + 24, 1);
        free(rows[i].name); free(rows[i].path);
    }
    free(rows);
    return reply;
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
    case OP_PLACES:
        r = list_places();
        break;
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
