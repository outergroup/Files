#include "Runtime.h"
#include "AppResources.h"
#include "../Backend/Environment.h"
#include "../Backend/FileList.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char *name, *path;
    bool directory;
    uint64_t size, modified;
} Entry;
static HANDLE stopped;
static char *web_root;
static char *local_path(const char *requested) {
    char *home = NULL, *expanded = NULL;
    if (!requested || !*requested || !strcmp(requested, "~") || !strncmp(requested, "~/", 2) ||
        !strncmp(requested, "~\\", 2)) {
        home = win_folder(CSIDL_PROFILE);
        if (!home)
            return NULL;
        expanded = requested && requested[0] == '~' && requested[1] ? win_join(home, requested + 2)
                                                                    : win_strdup(home);
        requested = expanded;
    }
    bool unc = strlen(requested) >= 5 && (!strncmp(requested, "\\\\", 2) || !strncmp(requested, "//", 2));
    bool valid = unc || (strlen(requested) >= 3 && isalpha((unsigned char)requested[0]) && requested[1] == ':' &&
                 (requested[2] == '/' || requested[2] == '\\'));
    if (!strncmp(requested, "\\\\.\\", 4) || !strncmp(requested, "\\\\?\\", 4)) valid = false;
    if (valid)
        for (const unsigned char *p = (const unsigned char *)requested + 3; *p; p++)
            if (*p < 32 || strchr(":*?\"<>|", *p))
                valid = false;
    wchar_t *wide = valid ? win_wide(requested) : NULL;
    wchar_t full[32768];
    DWORD n = wide ? GetFullPathNameW(wide, 32768, full, NULL) : 0;
    free(wide);
    free(home);
    free(expanded);
    if (!n || n >= 32768)
        return NULL;
    char *path = win_utf8(full);
    if (!path)
        return NULL;
    for (char *p = path; *p; p++)
        if (*p == '\\')
            *p = '/';
    size_t length = strlen(path);
    while (length > 3 && path[length - 1] == '/')
        path[--length] = 0;
    const char *segment = path + 3;
    while (*segment) {
        const char *end = strchr(segment, '/');
        if (!end)
            end = segment + strlen(segment);
        const char *dot = memchr(segment, '.', (size_t)(end - segment));
        const char *stem_end = dot ? dot : end;
        while (stem_end > segment && stem_end[-1] == ' ')
            stem_end--;
        size_t size = (size_t)(stem_end - segment);
        if ((size == 3 && (!_strnicmp(segment, "CON", 3) || !_strnicmp(segment, "PRN", 3) ||
                           !_strnicmp(segment, "AUX", 3) || !_strnicmp(segment, "NUL", 3))) ||
            (size == 4 && (!_strnicmp(segment, "COM", 3) || !_strnicmp(segment, "LPT", 3)) &&
             segment[3] >= '1' && segment[3] <= '9')) {
            free(path);
            return NULL;
        }
        segment = *end ? end + 1 : end;
    }
    return path;
}
static uint64_t modified_ms(FILETIME time) {
    ULARGE_INTEGER n;
    n.LowPart = time.dwLowDateTime;
    n.HighPart = time.dwHighDateTime;
    return n.QuadPart > 116444736000000000ULL ? (n.QuadPart - 116444736000000000ULL) / 10000 : 0;
}
static void free_entries(Entry *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);
}
static int compare_entries(const void *lhs, const void *rhs) {
    const Entry *a = lhs, *b = rhs;
    if (a->directory != b->directory)
        return a->directory ? -1 : 1;
    return _stricmp(a->name, b->name);
}
static void read_entry(const void *context, size_t i, FileListEntry *out) {
    const Entry *entry = &((const Entry *)context)[i];
    out->name = entry->name;
    out->path = entry->path;
    out->directory = entry->directory;
    out->size = entry->size;
    out->modified_ms = entry->modified;
}
static char *requested_path(HttpRequest *r, uint32_t magic) {
    if (!strcmp(r->method, "POST")) {
        if (r->body.length < 16 || read_uint32_le((unsigned char *)r->body.data) != magic ||
            read_uint32_le((unsigned char *)r->body.data + 4) != 1 ||
            read_uint32_le((unsigned char *)r->body.data + 8) < 16)
            return NULL;
        return payload_string(r->body.data, r->body.length, 8);
    }
    char *value = http_query(r, "path");
    return value ? value : win_strdup("~");
}
static void listing(Connection *c, HttpRequest *r, const char *path) {
    char *pattern = win_join(path, "*");
    wchar_t *wide = win_wide(pattern);
    free(pattern);
    WIN32_FIND_DATAW data;
    HANDLE find = wide ? FindFirstFileW(wide, &data) : INVALID_HANDLE_VALUE;
    DWORD error = GetLastError();
    free(wide);
    if (find == INVALID_HANDLE_VALUE) {
        http_windows_error(c, r, error);
        return;
    }
    size_t count = 0, capacity = 64;
    Entry *entries = win_alloc(capacity * sizeof(*entries));
    bool ok = true;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
            continue;
        if (count == 50000) {
            error = ERROR_BUFFER_OVERFLOW;
            ok = false;
            break;
        }
        if (count == capacity) {
            capacity *= 2;
            Entry *replacement = realloc(entries, capacity * sizeof(*entries));
            if (!replacement) {
                error = ERROR_NOT_ENOUGH_MEMORY;
                ok = false;
                break;
            }
            entries = replacement;
        }
        Entry entry = {0};
        entry.name = win_utf8(data.cFileName);
        if (!entry.name) {
            error = ERROR_NO_UNICODE_TRANSLATION;
            ok = false;
            break;
        }
        entry.path = win_join(path, entry.name);
        entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = entry.directory ? 0 : ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        entry.modified = modified_ms(data.ftLastWriteTime);
        entries[count++] = entry;
    } while (FindNextFileW(find, &data));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) {
        error = GetLastError();
        ok = false;
    }
    FindClose(find);
    char *home = local_path("~");
    if (ok && home && !_stricmp(path, home)) {
        char *desktop = win_folder(CSIDL_DESKTOPDIRECTORY);
        char *canonical = desktop ? local_path(desktop) : NULL;
        free(desktop);
        wchar_t *desktop_wide = canonical ? win_wide(canonical) : NULL;
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (desktop_wide && GetFileAttributesExW(desktop_wide, GetFileExInfoStandard, &attr) &&
            (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            size_t index = count;
            for (size_t i = 0; i < count; i++)
                if (!_stricmp(entries[i].name, "Desktop")) {
                    index = i;
                    break;
                }
            if (index == count) {
                if (count == capacity) {
                    Entry *replacement = realloc(entries, (capacity + 1) * sizeof(*entries));
                    if (!replacement) {
                        ok = false;
                        error = ERROR_NOT_ENOUGH_MEMORY;
                    } else {
                        entries = replacement;
                        capacity++;
                    }
                }
                if (ok) {
                    memset(&entries[count], 0, sizeof(*entries));
                    count++;
                }
            }
            if (ok) {
                free(entries[index].name);
                free(entries[index].path);
                entries[index] =
                    (Entry){win_strdup("Desktop"), canonical, true, 0, modified_ms(attr.ftLastWriteTime)};
                canonical = NULL;
            }
        }
        free(desktop_wide);
        free(canonical);
    }
    free(home);
    if (!ok) {
        free_entries(entries, count);
        http_windows_error(c, r, error);
        return;
    }
    qsort(entries, count, sizeof(*entries), compare_entries);
    char *parent = win_strdup(path);
    if (strlen(parent) <= 3)
        *parent = 0;
    else {
        char *slash = strrchr(parent, '/');
        if (slash == parent + 2)
            slash[1] = 0;
        else
            *slash = 0;
    }
    size_t length;
    unsigned char *bytes = file_list_encode(path, parent, count, entries, read_entry, &length);
    if (bytes)
        http_send(c, r, 200, "application/octet-stream", bytes, length);
    else
        http_text(c, r, 500, "Directory listing could not be encoded.");
    free(bytes);
    free(parent);
    free_entries(entries, count);
}
static char *destination(HttpRequest *r) {
    char *directory = NULL, *name = NULL;
    if (!strcmp(r->path, "/api/mkdir") && r->body.length) {
        if (r->body.length < 24 || read_uint32_le((unsigned char *)r->body.data) != 0x51444d46 ||
            read_uint32_le((unsigned char *)r->body.data + 4) != 1)
            return NULL;
        directory = payload_string(r->body.data, r->body.length, 8);
        name = payload_string(r->body.data, r->body.length, 16);
    } else {
        directory = http_query(r, "directory");
        name = http_query(r, "name");
    }
    char *result = NULL;
    bool valid = directory && name && *name && strcmp(name, ".") && strcmp(name, "..") &&
                 !strpbrk(name, "/\\:*?\"<>|");
    if (valid) {
        size_t n = strlen(name);
        if (name[n - 1] == '.' || name[n - 1] == ' ')
            valid = false;
        for (const unsigned char *p = (unsigned char *)name; *p; p++)
            if (*p < 32)
                valid = false;
    }
    if (valid) {
        char *base = local_path(directory);
        wchar_t *wide = base ? win_wide(base) : NULL;
        DWORD attr = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
        free(wide);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            char *joined = win_join(base, name);
            result = local_path(joined);
            free(joined);
        }
        free(base);
    }
    free(directory);
    free(name);
    return result;
}
static void mutation(Connection *c, HttpRequest *r) {
    if (strcmp(r->method, "POST")) {
        http_text(c, r, 405, "Use POST for file changes.");
        return;
    }
    char *path = destination(r);
    if (!path) {
        http_text(c, r, 400, "Invalid destination name or directory.");
        return;
    }
    wchar_t *wide = win_wide(path);
    free(path);
    bool ok = false;
    DWORD error = ERROR_INVALID_NAME;
    if (wide && !strcmp(r->path, "/api/mkdir")) {
        ok = CreateDirectoryW(wide, NULL) != 0;
        error = GetLastError();
        if (!ok && error == ERROR_ALREADY_EXISTS) {
            DWORD attr = GetFileAttributesW(wide);
            ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
        }
    } else if (wide) {
        HANDLE file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        error = GetLastError();
        if (file != INVALID_HANDLE_VALUE) {
            size_t offset = 0;
            ok = true;
            while (ok && offset < r->body.length) {
                DWORD count, n = (DWORD)(r->body.length - offset);
                if (n > 65536)
                    n = 65536;
                ok = WriteFile(file, r->body.data + offset, n, &count, NULL) && count;
                if (ok)
                    offset += count;
            }
            if (ok)
                ok = FlushFileBuffers(file) != 0;
            error = GetLastError();
            CloseHandle(file);
        }
    }
    free(wide);
    if (ok)
        http_text(c, r, 200, "ok\n");
    else
        http_windows_error(c, r, error);
}
static void serve(Connection *c, void *context) {
    (void)context;
    HttpRequest r = {0};
    if (!http_read(c, &r) || !http_authorize(c, &r)) {
        http_free(&r);
        return;
    }
    char *directory = win_executable_directory();
    bool resources = app_resources(c, &r, directory, "FilesContent.bundle", "{\"filesAPIPath\":\"/api/files\",\"openersAPIPath\":\"/api/openers\",\"previewAPIPath\":\"/api/preview\",\"rootPath\":\"~\"}");
    free(directory);
    if (resources) { http_free(&r); return; }
    if (!strcmp(r.path, "/api/environments") && !strcmp(r.method, "POST")) {
        size_t length = 0;
        char *body = environment_request(r.body.data, r.body.length, &length);
        if (body) http_send(c, &r, 200, "application/octet-stream", body, length);
        else http_text(c, &r, 400, "Invalid environment request.");
        free(body);
    } else if (!strcmp(r.path, "/api/files") || !strcmp(r.path, "/api/preview") ||
        !strcmp(r.path, "/api/download")) {
        char *requested = requested_path(&r, 0x51465046), *path = requested ? local_path(requested) : NULL;
        free(requested);
        if (!path)
            http_text(c, &r, 400, "Choose an absolute local drive path.");
        else if (!strcmp(r.path, "/api/files"))
            listing(c, &r, path);
        else
            http_file(c, &r, path, !strcmp(r.path, "/api/preview"), !strcmp(r.path, "/api/download"));
        free(path);
    } else if (!strcmp(r.path, "/api/openers")) {
        char *path = requested_path(&r, 0x514f464f);
        if (!path || !*path) {
            free(path);
            http_text(c, &r, 400, "Invalid file opener request.");
            http_free(&r);
            return;
        }
        char *pipe = win_pipe("org.outershell.Plaintext");
        bool available = pipe && win_pipe_is_allowed(pipe);
        StringBuilder p = payload(available ? 80 : 32);
        payload_u32(&p, 0, 0x504f464f); payload_u32(&p, 4, 2);
        payload_u32(&p, 8, available ? 1 : 0); payload_u32(&p, 12, 48);
        payload_u32(&p, 16, 32); payload_u32(&p, 20, (uint32_t)p.length);
        if (available) {
            StringBuilder url = {0}; sb_append(&url, "/?file=");
            const char *hex = "0123456789ABCDEF";
            for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
                if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-._~", *c)) sb_append_n(&url, (const char *)c, 1);
                else { char escaped[] = {'%', hex[*c >> 4], hex[*c & 15]}; sb_append_n(&url, escaped, 3); }
            }
            const char *fields[] = {"public.text", "org.outershell.Plaintext", "Plaintext", pipe, url.data, ""};
            for (size_t i = 0; i < 6; i++) payload_text(&p, 32 + i * 8, fields[i]);
            free(url.data);
        }
        payload_u32(&p, 24, (uint32_t)p.length);
        http_send(c, &r, 200, "application/octet-stream", p.data, p.length);
        free(path); free(pipe); free(p.data);
    } else if (!strcmp(r.path, "/api/mkdir") || !strcmp(r.path, "/api/upload"))
        mutation(c, &r);
    else {
        HttpRequest resource = r;
        if (!strncmp(r.path, "/web/", 5)) resource.path = r.path + 4;
        http_static(c, &resource, web_root);
    }
    http_free(&r);
}
static BOOL WINAPI console_control(DWORD event) {
    (void)event;
    SetEvent(stopped);
    return TRUE;
}
int wmain(int argc, wchar_t **argv) {
    (void)argv;
    if (argc != 1) {
        fputs("Unsupported Files option.\n", stderr);
        return 1;
    }
    stopped = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stopped)
        return 1;
    SetConsoleCtrlHandler(console_control, TRUE);
    char *directory = win_executable_directory(), *pipe = win_pipe("org.outershell.Files");
    if (!directory || !pipe)
        return 1;
    web_root = win_join(directory, "web");
    free(directory);
    PipeServer *server = pipe_server_start(pipe, serve, NULL);
    free(pipe);
    if (!server) {
        fputs("Files named pipe could not start.\n", stderr);
        return 1;
    }
    pipe_servers_wait_idle(stopped, 300000);
    pipe_server_stop(server);
    CloseHandle(stopped);
    free(web_root);
    return 0;
}
