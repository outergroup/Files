#include "helper.h"
#include <sys/file.h>
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

static void place_add(FileEntry **rows, size_t *count, const char *path, const char *name, bool personal) {
    char resolved[PATH_MAX];
    struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st) || !S_ISDIR(st.st_mode)) return;
    for (size_t i = 0; i < *count; i++) {
        if (!strcmp((*rows)[i].path, resolved)) {
            if (name && *name) { free((*rows)[i].name); (*rows)[i].name = strdup(name); }
            if (personal) strcpy((*rows)[i].mode, "favorite");
            return;
        }
    }
    if (*count >= 256) return;
    FileEntry *next = realloc(*rows, (*count + 1) * sizeof(**rows));
    if (!next) { fail("out of memory"); return; }
    *rows = next;
    const char *base = strrchr(resolved, '/');
    next[*count] = (FileEntry){.path = strdup(resolved),
        .name = strdup(name && *name ? name : base && base[1] ? base + 1 : resolved)};
    if (personal) strcpy(next[*count].mode, "favorite");
    (*count)++;
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
static bool personal_places_path(char path[PATH_MAX], bool create) {
    const char *config = getenv("XDG_CONFIG_HOME");
    char home[PATH_MAX];
    if (!config || *config != '/') {
        if (!expand_path("~", home)) return false;
        if (snprintf(path, PATH_MAX, "%s/.config/outer-files", home) >= PATH_MAX) {
            fail("favorites path is too long"); return false;
        }
    } else if (snprintf(path, PATH_MAX, "%s/outer-files", config) >= PATH_MAX) {
        fail("favorites path is too long"); return false;
    }
    if (create) {
        for (char *p = path + 1; ; p++) {
            if (*p && *p != '/') continue;
            char saved = *p;
            *p = 0;
            int result = mkdir(path, 0700);
            *p = saved;
            if (result && errno != EEXIST) { fail("Cannot create favorites folder: %s", strerror(errno)); return false; }
            if (!saved) break;
        }
    }
    size_t length = strlen(path);
    if (length + sizeof("/places.tsv") > PATH_MAX) { fail("favorites path is too long"); return false; }
    strcpy(path + length, "/places.tsv");
    return true;
}

static void read_places(const char *path, FileEntry **rows, size_t *count, bool personal) {
    FILE *config = fopen(path, "r");
    if (config) {
        char *line = NULL; size_t capacity = 0;
        while (getline(&line, &capacity, config) >= 0) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line != '/') continue;
            char *label = strchr(line, '\t');
            if (!label) continue;
            *label++ = 0;
            place_add(rows, count, line, label, personal);
        }
        free(line); fclose(config);
    }
}

static bool place_hidden(char **hidden, size_t hidden_count, const char *path) {
    for (size_t i = 0; i < hidden_count; i++)
        if (!strcmp(hidden[i], path)) return true;
    return false;
}

static void read_hidden_places(const char *path, char ***hidden, size_t *count) {
    FILE *config = fopen(path, "r");
    if (!config) return;
    char *line = NULL; size_t capacity = 0;
    while (getline(&line, &capacity, config) >= 0) {
        line[strcspn(line, "\t\r\n")] = 0;
        if (line[0] != '!' || line[1] != '/') continue;
        char **next = realloc(*hidden, (*count + 1) * sizeof(**hidden));
        if (!next) break;
        *hidden = next;
        if (((*hidden)[*count] = strdup(line + 1))) (*count)++;
    }
    free(line); fclose(config);
}

/* Home, directory mounts, and the system places file, each marked "place" except Home. */
static void builtin_places(FileEntry **rows, size_t *count) {
    char home[PATH_MAX];
    if (expand_path("~", home)) place_add(rows, count, home, "Home", false);
    size_t first = *count;
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
            place_add(rows, count, field, NULL, false);
        }
        free(line); fclose(mounts);
    }
#endif
    read_places("/etc/outer-files/places.tsv", rows, count, false);
    for (size_t i = first; i < *count; i++)
        if (!(*rows)[i].mode[0]) strcpy((*rows)[i].mode, "place");
}

static void free_places(FileEntry *rows, size_t count) {
    for (size_t i = 0; i < count; i++) { free(rows[i].name); free(rows[i].path); }
    free(rows);
}

/* Returns true when `resolved` is a built-in place other than Home. */
static bool is_hideable_builtin_place(const char *resolved) {
    FileEntry *rows = NULL;
    size_t count = 0;
    bool found = false;
    builtin_places(&rows, &count);
    for (size_t i = 0; i < count && !found; i++)
        found = !strcmp(rows[i].path, resolved) && !strcmp(rows[i].mode, "place");
    free_places(rows, count);
    return found;
}

/*
 * Rewrites the personal places file without any favorite or hidden line naming `path`
 * (directly or through a symbolic link), then appends `append` if it is not NULL.
 * Sets `*had_favorite` when a favorite line was dropped.
 */
static bool rewrite_personal_places(const char *path, const char *resolved, const char *append, bool *had_favorite) {
    char config[PATH_MAX];
    *had_favorite = false;
    if (!personal_places_path(config, true)) return false;
    int fd = open(config, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) { fail("Cannot open favorites: %s", strerror(errno)); return false; }
    if (flock(fd, LOCK_EX)) { fail("Cannot lock favorites: %s", strerror(errno)); close(fd); return false; }
    FILE *file = fdopen(fd, "r+");
    if (!file) { fail("Cannot read favorites: %s", strerror(errno)); close(fd); return false; }
    char *line = NULL, *kept = NULL; size_t capacity = 0, kept_length = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char entry[PATH_MAX], existing[PATH_MAX];
        bool hidden = line[0] == '!';
        const char *start = hidden ? line + 1 : line;
        size_t path_length = strcspn(start, "\t\r\n");
        bool matches = false;
        if (*start == '/' && (hidden || start[path_length] == '\t') && path_length < sizeof(entry)) {
            memcpy(entry, start, path_length);
            entry[path_length] = 0;
            matches = !strcmp(entry, resolved) || !strcmp(entry, path) ||
                (realpath(entry, existing) && !strcmp(existing, resolved));
        }
        if (matches) { if (!hidden) *had_favorite = true; continue; }
        if (length && line[length - 1] != '\n') line[length++] = '\n';
        char *grown = realloc(kept, kept_length + (size_t)length + 1);
        if (!grown) { fail("out of memory"); break; }
        kept = grown;
        memcpy(kept + kept_length, line, (size_t)length);
        kept_length += (size_t)length;
    }
    free(line);
    if (!failure[0] && ferror(file)) fail("Cannot read favorites: %s", strerror(errno));
    if (!failure[0] && append) {
        size_t append_length = strlen(append);
        char *grown = realloc(kept, kept_length + append_length + 1);
        if (!grown) fail("out of memory");
        else {
            kept = grown;
            memcpy(kept + kept_length, append, append_length);
            kept_length += append_length;
        }
    }
    if (!failure[0]) {
        if (fseek(file, 0, SEEK_SET) || (kept_length && fwrite(kept, 1, kept_length, file) != kept_length) ||
            fflush(file) || ftruncate(fd, (off_t)kept_length) || fsync(fd)) fail("Cannot save favorites: %s", strerror(errno));
    }
    free(kept);
    if (fclose(file) && !failure[0]) fail("Cannot close favorites: %s", strerror(errno));
    return !failure[0];
}

static BinaryReply add_place(const char *path) {
    BinaryReply reply = {0};
    char resolved[PATH_MAX];
    struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st)) {
        fail("Cannot add favorite: %s", strerror(errno)); return reply;
    }
    if (!S_ISDIR(st.st_mode)) { fail("Only folders can be added to Favorites"); return reply; }
    if (strpbrk(resolved, "\t\r\n")) { fail("Folder paths containing tabs or newlines cannot be saved as favorites"); return reply; }
    const char *name = strrchr(resolved, '/');
    name = name && name[1] ? name + 1 : resolved;
    char *line = NULL;
    if (asprintf(&line, "%s\t%s\n", resolved, name) < 0) { fail("out of memory"); return reply; }
    bool had_favorite;
    bool saved = rewrite_personal_places(path, resolved, line, &had_favorite);
    free(line);
    if (saved) {
        reply = response_create(OP_ADD_PLACE, 0, 0);
        binary_string(&reply.metadata, 48, resolved);
    }
    return reply;
}

/* Drops the folder's favorite line and hides it if it is a built-in place such as a mount. */
static BinaryReply remove_place(const char *path) {
    BinaryReply reply = {0};
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) snprintf(resolved, sizeof(resolved), "%s", path);
    if (strpbrk(resolved, "\r\n")) { fail("This folder is not in your Favorites"); return reply; }
    bool builtin = is_hideable_builtin_place(resolved);
    char *line = NULL;
    if (builtin && asprintf(&line, "!%s\n", resolved) < 0) { fail("out of memory"); return reply; }
    bool had_favorite;
    bool saved = rewrite_personal_places(path, resolved, line, &had_favorite);
    free(line);
    if (saved && !had_favorite && !builtin) { fail("This folder is not in your Favorites"); return reply; }
    if (saved) {
        reply = response_create(OP_REMOVE_PLACE, 0, 0);
        binary_string(&reply.metadata, 48, resolved);
    }
    return reply;
}

static BinaryReply list_places(void) {
    FileEntry *rows = NULL;
    size_t count = 0;
    char **hidden = NULL;
    size_t hidden_count = 0;
    builtin_places(&rows, &count);
    char personal[PATH_MAX];
    if (personal_places_path(personal, false)) {
        read_hidden_places(personal, &hidden, &hidden_count);
        read_places(personal, &rows, &count, true);
    }
    size_t shown = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(rows[i].mode, "place") && place_hidden(hidden, hidden_count, rows[i].path)) {
            free(rows[i].name); free(rows[i].path);
            continue;
        }
        rows[shown++] = rows[i];
    }
    count = shown;
    for (size_t i = 0; i < hidden_count; i++) free(hidden[i]);
    free(hidden);
    BinaryReply reply = response_create(OP_PLACES, (uint32_t)count, FILE_ROW_SIZE);
    for (size_t i = 0; i < count; i++) {
        size_t offset = RESPONSE_SIZE + i * FILE_ROW_SIZE;
        binary_string(&reply.metadata, offset, rows[i].name);
        binary_string(&reply.metadata, offset + 8, rows[i].path);
        binary_string(&reply.metadata, offset + 16, rows[i].mode);
        put_u32(reply.metadata.data + offset + 24, 1);
    }
    free_places(rows, count);
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
    case OP_ADD_PLACE:
        r = add_place(path);
        break;
    case OP_REMOVE_PLACE:
        r = remove_place(path);
        break;
    case OP_PLACES:
        r = list_places();
        break;
    case OP_LIST: {
        char resolved[PATH_MAX];
        if (!realpath(path, resolved)) fail("%s: %s", path, strerror(errno));
        else r = list_files(resolved);
        break;
    }
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
