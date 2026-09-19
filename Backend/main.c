#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "HTTPCache.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
extern int launch_activate_socket(const char *name, int **fds, size_t *cnt);
#endif

#define DEFAULT_PORT 7354
#define READ_BUFFER_SIZE 8192
#define FILE_TEXT_PREVIEW_MAX_BYTES (256 * 1024)
#define FILE_MEDIA_PREVIEW_MAX_BYTES (16 * 1024 * 1024)

static const char *kBundleUrlPath = "/bundles/FilesContent";
static const char *kBundleUrlPathMacosArm = "/bundles/FilesContent/macos-arm";
static const char *kBundleUrlPathMacosX86 = "/bundles/FilesContent/macos-x86";
static const char *kBundleFilePathMacosArm = "bundles/FilesContent.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/FilesContent.bundle.macos-x86.aar";

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_web_root[PATH_MAX] = "";
static char g_backend_label[256] = "org.outershell.Files";
static char g_outershelld_api_socket_path[PATH_MAX] = "";
static char g_app_icon_path[PATH_MAX] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static bool g_systemd_socket_activation = false;
static bool g_listen_socket_is_launchd_owned = false;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_listener_fd = -1;

#ifdef __APPLE__
static bool remove_last_path_component(char *path) {
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    if (slash == path) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return true;
}

static void configure_resource_paths_from_app_bundle(void) {
    char executable_path[PATH_MAX];
    uint32_t executable_path_size = sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &executable_path_size) != 0) {
        return;
    }

    char resolved_path[PATH_MAX];
    const char *path = realpath(executable_path, resolved_path) ? resolved_path : executable_path;
    char macos_dir[PATH_MAX];
    snprintf(macos_dir, sizeof(macos_dir), "%s", path);
    if (!remove_last_path_component(macos_dir)) return;

    char contents_dir[PATH_MAX];
    snprintf(contents_dir, sizeof(contents_dir), "%s", macos_dir);
    if (!remove_last_path_component(contents_dir)) return;

    char arm_bundle_path[PATH_MAX];
    char x86_bundle_path[PATH_MAX];
    snprintf(arm_bundle_path, sizeof(arm_bundle_path),
             "%s/Resources/bundles/FilesContent.bundle.macos-arm.aar", contents_dir);
    snprintf(x86_bundle_path, sizeof(x86_bundle_path),
             "%s/Resources/bundles/FilesContent.bundle.macos-x86.aar", contents_dir);

    struct stat st;
    if (stat(arm_bundle_path, &st) == 0 && S_ISREG(st.st_mode) &&
        stat(x86_bundle_path, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm), "%s", arm_bundle_path);
        snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86), "%s", x86_bundle_path);
    }

    char icon_path[PATH_MAX];
    snprintf(icon_path, sizeof(icon_path), "%s/Resources/app-icon.png", contents_dir);
    if (stat(icon_path, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(g_app_icon_path, sizeof(g_app_icon_path), "%s", icon_path);
    }
    snprintf(g_web_root, sizeof(g_web_root), "%s/Resources/web", contents_dir);
}
#endif

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

typedef struct {
    char path[PATH_MAX];
    char name[NAME_MAX + 1];
    bool is_directory;
    uint64_t size;
    double modified;
    mode_t mode;
    uint32_t access_flags;
} FileEntry;

enum {
    FILE_ENTRY_ACCESS_USER_READ = 1u << 0,
    FILE_ENTRY_ACCESS_USER_WRITE = 1u << 1
};

typedef struct {
    bool resolved;
    uid_t uid;
    gid_t primary_gid;
    gid_t *groups;
    int group_count;
} RequesterAccessContext;

typedef struct {
    int fd;
    char request[READ_BUFFER_SIZE];
    size_t request_len;
    size_t header_len;
    size_t content_length;
    unsigned char *body;
    size_t body_len;
    time_t accepted_at;
} HttpClient;

static bool query_value(const char *query, const char *name, char *dst, size_t dst_size);
static void resolve_requested_path(const char *requested, char *resolved, size_t resolved_size);

static const char *path_extension(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *dot = strrchr(name, '.');
    return dot && dot[1] != '\0' ? dot + 1 : "";
}

static const char *preview_content_type_for_path(const char *path) {
    const char *ext = path_extension(path);
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";
    if (strcasecmp(ext, "tif") == 0 || strcasecmp(ext, "tiff") == 0) return "image/tiff";
    if (strcasecmp(ext, "bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, "heic") == 0) return "image/heic";
    if (strcasecmp(ext, "heif") == 0) return "image/heif";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "icns") == 0) return "image/icns";
    if (strcasecmp(ext, "svg") == 0 || strcasecmp(ext, "svgz") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    return "text/plain; charset=utf-8";
}

static bool preview_content_type_is_text(const char *content_type) {
    return strncasecmp(content_type, "text/", 5) == 0;
}

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
    if (g_listener_fd >= 0) {
        close((int)g_listener_fd);
    }
}

static void write_uint32_le(unsigned char *dst, uint32_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
    dst[2] = (unsigned char)((value >> 16) & 0xffu);
    dst[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void write_uint16_le(unsigned char *dst, uint16_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void write_uint64_le(unsigned char *dst, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }
}

static bool queue_all(int fd, const void *data, size_t len) {
    const char *bytes = (const char *)data;
    while (len > 0) {
        ssize_t written = write(fd, bytes, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        len -= (size_t)written;
    }
    return true;
}

static void send_response(int fd, int status, const char *status_text, const char *content_type,
                          const void *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n"
                              "\r\n",
                              status, status_text, content_type, body_len);
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        queue_all(fd, header, (size_t)header_len);
    }
    if (body && body_len > 0) {
        queue_all(fd, body, body_len);
    }
}

static void send_text_response(int fd, int status, const char *message) {
    const char *status_text = status == 200 ? "OK" :
                              status == 400 ? "Bad Request" :
                              status == 404 ? "Not Found" :
                              status == 413 ? "Payload Too Large" :
                              status == 502 ? "Bad Gateway" :
                              status == 500 ? "Internal Server Error" : "Error";
    send_response(fd, status, status_text, "text/plain; charset=utf-8", message, strlen(message));
}

static void send_cached_header(int fd,
                               int status,
                               const char *content_type,
                               size_t content_length,
                               const char *etag,
                               const char *last_modified) {
    char header[1024];
    size_t header_length = outer_http_cache_response_header(header, sizeof(header), status,
                                                            content_type, content_length,
                                                            etag, last_modified);
    if (header_length > 0) queue_all(fd, header, header_length);
}

static void send_cached_memory_response(int fd,
                                        const char *request,
                                        size_t request_header_length,
                                        const char *content_type,
                                        const void *body,
                                        size_t body_length,
                                        bool send_body) {
    time_t last_modified = outer_http_cache_server_start_time();
    char etag[96], last_modified_text[64];
    outer_http_cache_memory_etag(body, body_length, etag, sizeof(etag));
    outer_http_cache_format_date(last_modified, last_modified_text, sizeof(last_modified_text));
    if (outer_http_cache_not_modified(request, request_header_length, etag, &last_modified)) {
        send_cached_header(fd, 304, content_type, 0, etag, last_modified_text);
        return;
    }
    send_cached_header(fd, 200, content_type, body_length, etag, last_modified_text);
    if (send_body && body_length > 0) queue_all(fd, body, body_length);
}

static void send_outer_descriptor(int fd,
                                  const char *request,
                                  size_t request_header_length,
                                  bool send_body) {
    const char *plugin_json = "{\"filesAPIPath\":\"/api/files\",\"openersAPIPath\":\"/api/openers\",\"previewAPIPath\":\"/api/preview\",\"rootPath\":\"~\"}";
    size_t path_len = strlen(kBundleUrlPath);
    size_t plugin_len = strlen(plugin_json);
    size_t header_len = 40;
    size_t data_offset = header_len + path_len;
    size_t total_len = data_offset + plugin_len;
    unsigned char *payload = malloc(total_len);
    if (!payload) {
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    payload[0] = 'O';
    payload[1] = 'U';
    payload[2] = 'T';
    payload[3] = 'R';
    write_uint32_le(payload + 4, 1);
    write_uint64_le(payload + 8, (uint64_t)header_len);
    write_uint64_le(payload + 16, (uint64_t)path_len);
    write_uint64_le(payload + 24, (uint64_t)data_offset);
    write_uint64_le(payload + 32, (uint64_t)plugin_len);
    memcpy(payload + header_len, kBundleUrlPath, path_len);
    memcpy(payload + data_offset, plugin_json, plugin_len);

    send_cached_memory_response(fd, request, request_header_length,
                                "application/vnd.outerframe", payload, total_len,
                                send_body);
    free(payload);
}

static bool request_accepts_outerframe(const char *request, size_t request_header_length) {
    static const char header_name[] = "Outerframe-Accept:";
    static const char media_type[] = "application/vnd.outerframe";
    const char *cursor = request;
    const char *end = request + request_header_length;
    while (cursor < end) {
        const char *line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) line_end = end;
        if ((size_t)(line_end - cursor) >= sizeof(header_name) - 1 &&
            strncasecmp(cursor, header_name, sizeof(header_name) - 1) == 0) {
            const char *value = cursor + sizeof(header_name) - 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            size_t length = (size_t)(line_end - value);
            return length >= sizeof(media_type) - 1 &&
                   memmem(value, length, media_type, sizeof(media_type) - 1) != NULL;
        }
        cursor = line_end < end ? line_end + 2 : end;
    }
    return false;
}

static void send_web_file(int fd, const char *name, const char *content_type,
                          const char *request, size_t request_header_length, bool send_body) {
    const char *root = g_web_root[0] ? g_web_root : "web";
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) {
        send_text_response(fd, 404, "web asset not found\n");
        return;
    }
    int file_fd = open(path, O_RDONLY);
    struct stat st;
    if (file_fd < 0 || fstat(file_fd, &st) != 0 || st.st_size < 0 || !S_ISREG(st.st_mode)) {
        if (file_fd >= 0) close(file_fd);
        send_text_response(fd, 404, "web asset not found\n");
        return;
    }
    size_t length = (size_t)st.st_size;
    unsigned char *data = malloc(length ? length : 1);
    if (!data) { close(file_fd); send_text_response(fd, 500, "out of memory\n"); return; }
    size_t offset = 0;
    while (offset < length) {
        ssize_t got = read(file_fd, data + offset, length - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    if (offset != length) { free(data); send_text_response(fd, 500, "failed to read web asset\n"); return; }
    send_cached_memory_response(fd, request, request_header_length, content_type, data, length, send_body);
    free(data);
}

static void send_cached_bundle_file(int fd,
                                    const char *path,
                                    const char *request,
                                    size_t request_header_length,
                                    bool send_body) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "bundle not found at %s\n", path);
        send_text_response(fd, 404, message);
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        send_text_response(fd, 500, "failed to stat bundle\n");
        return;
    }
    char etag[96], last_modified[64];
    outer_http_cache_file_etag(&st, etag, sizeof(etag));
    outer_http_cache_format_date(st.st_mtime, last_modified, sizeof(last_modified));
    if (outer_http_cache_not_modified(request, request_header_length, etag, &st.st_mtime)) {
        close(file_fd);
        send_cached_header(fd, 304, "application/octet-stream", 0, etag, last_modified);
        return;
    }
    size_t size = (size_t)st.st_size;
    if (!send_body) {
        close(file_fd);
        send_cached_header(fd, 200, "application/octet-stream", size, etag, last_modified);
        return;
    }
    unsigned char *data = malloc(size > 0 ? size : 1);
    if (!data) {
        close(file_fd);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(file_fd, data + offset, size - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(data);
            close(file_fd);
            send_text_response(fd, 500, "failed to read bundle\n");
            return;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    if (offset != size) {
        free(data);
        send_text_response(fd, 500, "failed to read bundle\n");
        return;
    }
    send_cached_header(fd, 200, "application/octet-stream", size, etag, last_modified);
    if (size > 0) queue_all(fd, data, size);
    free(data);
}

static void send_bundle_file(int fd, const char *path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "bundle not found at %s\n", path);
        send_text_response(fd, 404, message);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0) {
        close(file_fd);
        send_text_response(fd, 500, "failed to stat bundle\n");
        return;
    }

    size_t size = (size_t)st.st_size;
    unsigned char *data = malloc(size);
    if (!data) {
        close(file_fd);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(file_fd, data + offset, size - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(data);
            close(file_fd);
            send_text_response(fd, 500, "failed to read bundle\n");
            return;
        }
        if (got == 0) {
            break;
        }
        offset += (size_t)got;
    }
    close(file_fd);
    send_response(fd, 200, "OK", "application/octet-stream", data, offset);
    free(data);
}

static bool sb_reserve(StringBuilder *builder, size_t additional) {
    if (builder->length + additional + 1 <= builder->capacity) {
        return true;
    }
    size_t new_capacity = builder->capacity ? builder->capacity * 2 : 4096;
    while (new_capacity < builder->length + additional + 1) {
        new_capacity *= 2;
    }
    char *new_data = realloc(builder->data, new_capacity);
    if (!new_data) {
        return false;
    }
    builder->data = new_data;
    builder->capacity = new_capacity;
    return true;
}

static bool sb_append_n(StringBuilder *builder, const char *text, size_t length) {
    if (!sb_reserve(builder, length)) {
        return false;
    }
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

static bool sb_append_u32_le(StringBuilder *builder, uint32_t value) {
    char bytes[4] = {
        (char)(value & 0xffu),
        (char)((value >> 8) & 0xffu),
        (char)((value >> 16) & 0xffu),
        (char)((value >> 24) & 0xffu)
    };
    return sb_append_n(builder, bytes, sizeof(bytes));
}

static bool sb_append_u16_le(StringBuilder *builder, uint16_t value) {
    char bytes[2] = {
        (char)(value & 0xffu),
        (char)((value >> 8) & 0xffu)
    };
    return sb_append_n(builder, bytes, sizeof(bytes));
}

static bool sb_append_u64_le(StringBuilder *builder, uint64_t value) {
    char bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = (char)((value >> (i * 8)) & 0xffu);
    }
    return sb_append_n(builder, bytes, sizeof(bytes));
}

static bool sb_append_zero(StringBuilder *builder, size_t length) {
    if (!sb_reserve(builder, length)) {
        return false;
    }
    memset(builder->data + builder->length, 0, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

static uint16_t read_u16_le_from_bytes(const unsigned char *data, size_t offset) {
    return (uint16_t)(((uint16_t)data[offset]) | ((uint16_t)data[offset + 1] << 8));
}

static uint32_t read_u32_le_at(const char *data, size_t offset) {
    const unsigned char *bytes = (const unsigned char *)(data + offset);
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_u32_le_at(char *data, size_t offset, uint32_t value) {
    data[offset] = (char)(value & 0xffu);
    data[offset + 1] = (char)((value >> 8) & 0xffu);
    data[offset + 2] = (char)((value >> 16) & 0xffu);
    data[offset + 3] = (char)((value >> 24) & 0xffu);
}

enum {
    FILE_PATH_REQUEST_BINARY_MAGIC = 0x51465046u,
    FILE_MKDIR_REQUEST_BINARY_MAGIC = 0x51444d46u,
    FILE_LIST_BINARY_MAGIC = 0x534c4646u,
    FILE_LIST_BINARY_VERSION = 2,
    FILE_LIST_BINARY_HEADER_SIZE = 48,
    FILE_LIST_BINARY_ROW_SIZE = 48,
    FILE_OPENERS_BINARY_MAGIC = 0x504f464fu,
    FILE_OPENERS_REQUEST_BINARY_MAGIC = 0x514f464fu,
    FILE_OPENERS_BINARY_VERSION = 2,
    FILE_OPENERS_BINARY_HEADER_SIZE = 32,
    FILE_OPENERS_API_ROW_SIZE = 44,
    FILE_OPENERS_API_STRING_FIELD_COUNT = 5,
    FILE_OPENERS_BINARY_ROW_SIZE = 48
};

enum {
    OUTERSHELLD_API_APP_ADD_REQUEST = 13,
    OUTERSHELLD_API_APP_REMOVE_REQUEST = 14,
    OUTERSHELLD_API_FILE_OPENERS_QUERY = 25,
    OUTERSHELLD_API_COMMAND_RESPONSE = 100,
    OUTERSHELLD_API_FILE_OPENERS_RESPONSE = 106,
    OUTERSHELLD_API_FILE_OPENERS_RESPONSE_FIXED_SIZE = 18,
    OUTERSHELLD_API_MAX_FRAME_SIZE = 16 * 1024 * 1024
};

static uint32_t read_u32_le_from_bytes(const unsigned char *data, size_t offset) {
    return ((uint32_t)data[offset]) |
           ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) |
           ((uint32_t)data[offset + 3] << 24);
}

static bool read_binary_string_ref(const unsigned char *data,
                                   size_t data_len,
                                   size_t ref_offset,
                                   char *out,
                                   size_t out_size) {
    if (!data || !out || out_size == 0 || ref_offset + 8 > data_len) return false;
    uint32_t string_offset = read_u32_le_from_bytes(data, ref_offset);
    uint32_t string_length = read_u32_le_from_bytes(data, ref_offset + 4);
    if ((size_t)string_offset > data_len || (size_t)string_length > data_len - (size_t)string_offset) return false;
    size_t copy_length = string_length;
    if (copy_length >= out_size) copy_length = out_size - 1;
    memcpy(out, data + string_offset, copy_length);
    out[copy_length] = '\0';
    return copy_length == (size_t)string_length;
}

static bool read_binary_path_request(const unsigned char *body,
                                     size_t body_len,
                                     uint32_t magic,
                                     char *path,
                                     size_t path_size) {
    if (!body || body_len < 16) return false;
    if (read_u32_le_from_bytes(body, 0) != magic || read_u32_le_from_bytes(body, 4) != 1) return false;
    return read_binary_string_ref(body, body_len, 8, path, path_size);
}

static bool read_binary_directory_name_request(const unsigned char *body,
                                               size_t body_len,
                                               uint32_t magic,
                                               char *directory,
                                               size_t directory_size,
                                               char *name,
                                               size_t name_size) {
    if (!body || body_len < 24) return false;
    if (read_u32_le_from_bytes(body, 0) != magic || read_u32_le_from_bytes(body, 4) != 1) return false;
    return read_binary_string_ref(body, body_len, 8, directory, directory_size) &&
           read_binary_string_ref(body, body_len, 16, name, name_size);
}

static bool read_binary_string_ref_view(const unsigned char *data,
                                        size_t data_len,
                                        size_t ref_offset,
                                        const unsigned char **out,
                                        size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!data || ref_offset + 8 > data_len) return false;
    uint32_t string_offset = read_u32_le_from_bytes(data, ref_offset);
    uint32_t string_length = read_u32_le_from_bytes(data, ref_offset + 4);
    if ((size_t)string_offset > data_len || (size_t)string_length > data_len - (size_t)string_offset) return false;
    if (out) *out = data + string_offset;
    if (out_len) *out_len = string_length;
    return true;
}

static bool append_binary_string_ref(StringBuilder *rows,
                                     StringBuilder *variable,
                                     const char *text) {
    size_t offset = variable->length;
    size_t length = text ? strlen(text) : 0;
    if (offset > UINT32_MAX || length > UINT32_MAX) {
        return false;
    }
    return sb_append_u32_le(rows, (uint32_t)offset) &&
           sb_append_u32_le(rows, (uint32_t)length) &&
           sb_append_n(variable, text ? text : "", length);
}

static bool append_binary_data_ref(StringBuilder *rows,
                                   StringBuilder *variable,
                                   const unsigned char *data,
                                   size_t length) {
    size_t offset = variable->length;
    if (offset > UINT32_MAX || length > UINT32_MAX) {
        return false;
    }
    return sb_append_u32_le(rows, (uint32_t)offset) &&
           sb_append_u32_le(rows, (uint32_t)length) &&
           sb_append_n(variable, (const char *)(data ? data : (const unsigned char *)""), length);
}

static void patch_string_refs_to_absolute_offsets(char *data, size_t data_len, uint32_t variable_offset) {
    for (size_t offset = 0; offset + 8 <= data_len; offset += 8) {
        uint32_t relative = read_u32_le_at(data, offset);
        write_u32_le_at(data, offset, relative + variable_offset);
    }
}

static void patch_file_list_row_string_refs(char *data, size_t data_len, uint32_t variable_offset) {
    for (size_t row_offset = 0; row_offset + FILE_LIST_BINARY_ROW_SIZE <= data_len; row_offset += FILE_LIST_BINARY_ROW_SIZE) {
        for (size_t field_offset = 0; field_offset < 24; field_offset += 8) {
            uint32_t relative = read_u32_le_at(data, row_offset + field_offset);
            write_u32_le_at(data, row_offset + field_offset, relative + variable_offset);
        }
    }
}

static void default_outershelld_api_socket_path(char *out, size_t out_size) {
    const char *env_path = getenv("OUTERSHELLD_API_SOCKET");
    if (env_path && env_path[0]) {
        snprintf(out, out_size, "%s", env_path);
        return;
    }
#ifdef __APPLE__
    if (geteuid() == 0) {
        snprintf(out, out_size, "/var/run/outershelld-api");
        return;
    }
    const char *tmp = getenv("DARWIN_USER_TEMP_DIR");
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    if (tmp && tmp[0]) {
        snprintf(out, out_size, "%s%soutershelld-api", tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/");
        return;
    }
    snprintf(out, out_size, "/tmp/outershelld-api-%d", (int)getuid());
#else
    if (geteuid() == 0) {
        snprintf(out, out_size, "/run/outershelld-api");
        return;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, out_size, "%s/outershelld-api", runtime);
        return;
    }
    snprintf(out, out_size, "/run/user/%d/outershelld-api", (int)getuid());
#endif
}

static bool write_exact_fd(int fd, const void *data, size_t length) {
    const char *bytes = data;
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        bytes += written;
        length -= (size_t)written;
    }
    return true;
}

static bool read_exact_fd(int fd, void *data, size_t length) {
    char *bytes = data;
    while (length > 0) {
        ssize_t got = read(fd, bytes, length);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        bytes += got;
        length -= (size_t)got;
    }
    return true;
}

static int connect_outershelld_api_socket(void) {
    if (!g_outershelld_api_socket_path[0]) return -1;
    if (strlen(g_outershelld_api_socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", g_outershelld_api_socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool api_message_append_string_ref_at(StringBuilder *message, size_t ref_offset, const char *text) {
    const char *safe_text = text ? text : "";
    size_t offset = message->length;
    size_t length = strlen(safe_text);
    if (offset > UINT32_MAX || length > UINT32_MAX || ref_offset + 8 > message->length) return false;
    if (!sb_append_n(message, safe_text, length)) return false;
    write_u32_le_at(message->data, ref_offset, (uint32_t)offset);
    write_u32_le_at(message->data, ref_offset + 4, (uint32_t)length);
    return true;
}

static bool send_outershelld_api_message(StringBuilder *message, StringBuilder *response) {
    if (!message || message->length > UINT32_MAX || !response) return false;
    int fd = connect_outershelld_api_socket();
    if (fd < 0) return false;

    unsigned char prefix[4];
    write_uint32_le(prefix, (uint32_t)message->length);
    bool ok = write_exact_fd(fd, prefix, sizeof(prefix)) &&
              write_exact_fd(fd, message->data, message->length);
    if (ok) {
        unsigned char response_length_bytes[4];
        ok = read_exact_fd(fd, response_length_bytes, sizeof(response_length_bytes));
        if (ok) {
            uint32_t response_length = read_u32_le_from_bytes(response_length_bytes, 0);
            ok = response_length <= OUTERSHELLD_API_MAX_FRAME_SIZE &&
                 sb_reserve(response, response_length) &&
                 read_exact_fd(fd, response->data, response_length);
            if (ok) {
                response->length = response_length;
                response->data[response->length] = '\0';
            }
        }
    }
    close(fd);
    return ok;
}

static bool send_outershelld_command_request(StringBuilder *message) {
    StringBuilder response = {0};
    bool ok = send_outershelld_api_message(message, &response);
    if (ok) {
        ok = response.length >= 6 &&
             read_u16_le_from_bytes((const unsigned char *)response.data, 0) == OUTERSHELLD_API_COMMAND_RESPONSE &&
             read_u32_le_from_bytes((const unsigned char *)response.data, 2) == 0;
    }
    free(response.data);
    return ok;
}

static bool send_app_add_to_outershelld(int port, const char *socket_path) {
    StringBuilder message = {0};
    bool ok = sb_append_zero(&message, 62) &&
              (write_uint16_le((unsigned char *)message.data, OUTERSHELLD_API_APP_ADD_REQUEST), true) &&
              (write_u32_le_at(message.data, 2, (uint32_t)(port > 0 ? port : 0)), true) &&
              api_message_append_string_ref_at(&message, 6, g_backend_label) &&
              api_message_append_string_ref_at(&message, 14, "Files") &&
              api_message_append_string_ref_at(&message, 22, "") &&
              api_message_append_string_ref_at(&message, 30, "") &&
              api_message_append_string_ref_at(&message, 38, g_app_icon_path) &&
              api_message_append_string_ref_at(&message, 46, "") &&
              api_message_append_string_ref_at(&message, 54, socket_path ? socket_path : "");
    ok = ok && send_outershelld_command_request(&message);
    free(message.data);
    return ok;
}

static bool send_app_remove_to_outershelld(int port, const char *socket_path) {
    StringBuilder message = {0};
    bool ok = sb_append_zero(&message, 30) &&
              (write_uint16_le((unsigned char *)message.data, OUTERSHELLD_API_APP_REMOVE_REQUEST), true) &&
              (write_u32_le_at(message.data, 2, (uint32_t)(port > 0 ? port : 0)), true) &&
              api_message_append_string_ref_at(&message, 6, g_backend_label) &&
              api_message_append_string_ref_at(&message, 14, "") &&
              api_message_append_string_ref_at(&message, 22, socket_path ? socket_path : "");
    ok = ok && send_outershelld_command_request(&message);
    free(message.data);
    return ok;
}

static bool query_file_openers_from_outershelld(const char *path,
                                                const char *content_type,
                                                const char *requester_user,
                                                StringBuilder *response) {
    StringBuilder message = {0};
    bool ok = sb_append_u16_le(&message, OUTERSHELLD_API_FILE_OPENERS_QUERY) &&
              sb_append_zero(&message, 24) &&
              api_message_append_string_ref_at(&message, 2, path) &&
              api_message_append_string_ref_at(&message, 10, content_type ? content_type : "") &&
              api_message_append_string_ref_at(&message, 18, requester_user ? requester_user : "");
    ok = ok && send_outershelld_api_message(&message, response);
    free(message.data);
    return ok;
}

static void send_openers_error_response(int fd, int status, const char *message) {
    const char *safe_message = message ? message : "openers unavailable\n";
    fprintf(stderr, "FilesBackend openers: %s", safe_message);
    if (safe_message[0] && safe_message[strlen(safe_message) - 1] != '\n') {
        fprintf(stderr, "\n");
    }
    send_text_response(fd, status, safe_message);
}

static void opener_owner_name_for_socket_path(const char *socket_path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!socket_path || !socket_path[0]) return;

    struct stat st;
    if (stat(socket_path, &st) != 0) return;

    struct passwd *pw = getpwuid(st.st_uid);
    if (pw && pw->pw_name && pw->pw_name[0]) {
        snprintf(out, out_size, "%s", pw->pw_name);
        return;
    }
    snprintf(out, out_size, "%u", (unsigned int)st.st_uid);
}

static void send_openers_response_for_path(int fd,
                                           const char *requested,
                                           const char *requester_user) {
    char path[PATH_MAX];
    resolve_requested_path(requested, path, sizeof(path));
    if (!g_outershelld_api_socket_path[0]) {
        send_openers_error_response(fd, 502, "openers unavailable: outershelld API socket is not configured\n");
        return;
    }

    StringBuilder api_response = {0};
    if (!query_file_openers_from_outershelld(path, "", requester_user, &api_response)) {
        free(api_response.data);
        send_openers_error_response(fd, 502, "openers unavailable: failed to query outershelld API\n");
        return;
    }
    if (api_response.length < OUTERSHELLD_API_FILE_OPENERS_RESPONSE_FIXED_SIZE ||
        read_u16_le_from_bytes((const unsigned char *)api_response.data, 0) != OUTERSHELLD_API_FILE_OPENERS_RESPONSE) {
        free(api_response.data);
        send_openers_error_response(fd, 502, "openers unavailable: outershelld returned an invalid opener response\n");
        return;
    }
    if (read_u32_le_from_bytes((const unsigned char *)api_response.data, 2) != 0) {
        char api_error[512] = "outershelld returned an opener query error";
        (void)read_binary_string_ref((const unsigned char *)api_response.data, api_response.length, 6, api_error, sizeof(api_error));
        char message[640];
        snprintf(message, sizeof(message), "openers unavailable: %s\n", api_error[0] ? api_error : "outershelld returned an opener query error");
        free(api_response.data);
        send_openers_error_response(fd, 502, message);
        return;
    }

    uint32_t api_row_count = read_u32_le_from_bytes((const unsigned char *)api_response.data, 14);
    if (api_row_count > (api_response.length - OUTERSHELLD_API_FILE_OPENERS_RESPONSE_FIXED_SIZE) / FILE_OPENERS_API_ROW_SIZE) {
        free(api_response.data);
        send_openers_error_response(fd, 502, "openers unavailable: outershelld opener response is truncated\n");
        return;
    }

    StringBuilder rows = {0};
    StringBuilder variable = {0};
    uint32_t row_count = 0;
    bool ok = true;
    const unsigned char *api_bytes = (const unsigned char *)api_response.data;
    for (uint32_t i = 0; ok && i < api_row_count; i++) {
        size_t api_row_offset = OUTERSHELLD_API_FILE_OPENERS_RESPONSE_FIXED_SIZE + (size_t)i * FILE_OPENERS_API_ROW_SIZE;
        const unsigned char *socket_path = NULL;
        size_t socket_path_length = 0;
        for (size_t field_index = 0; ok && field_index < FILE_OPENERS_API_STRING_FIELD_COUNT; field_index++) {
            size_t field_offset = field_index * 8;
            const unsigned char *value = NULL;
            size_t value_length = 0;
            ok = read_binary_string_ref_view(api_bytes, api_response.length, api_row_offset + field_offset, &value, &value_length) &&
                 append_binary_data_ref(&rows, &variable, value, value_length);
            if (ok && field_offset == 24) {
                socket_path = value;
                socket_path_length = value_length;
            }
        }
        if (ok) {
            char socket_path_buffer[PATH_MAX];
            size_t length = socket_path_length < sizeof(socket_path_buffer) - 1 ? socket_path_length : sizeof(socket_path_buffer) - 1;
            if (socket_path && length > 0) {
                memcpy(socket_path_buffer, socket_path, length);
            }
            socket_path_buffer[length] = '\0';
            char owner_name[128];
            opener_owner_name_for_socket_path(socket_path_buffer, owner_name, sizeof(owner_name));
            ok = append_binary_data_ref(&rows,
                                        &variable,
                                        (const unsigned char *)owner_name,
                                        strlen(owner_name));
        }
        if (ok) row_count++;
    }
    free(api_response.data);

    size_t variable_offset = FILE_OPENERS_BINARY_HEADER_SIZE + rows.length;
    size_t total_size = variable_offset + variable.length;
    if (!ok || rows.length > UINT32_MAX || variable.length > UINT32_MAX ||
        variable_offset > UINT32_MAX || total_size > UINT32_MAX) {
        free(rows.data);
        free(variable.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    patch_string_refs_to_absolute_offsets(rows.data, rows.length, (uint32_t)variable_offset);

    StringBuilder response = {0};
    ok = sb_append_u32_le(&response, FILE_OPENERS_BINARY_MAGIC) &&
         sb_append_u32_le(&response, FILE_OPENERS_BINARY_VERSION) &&
         sb_append_u32_le(&response, row_count) &&
         sb_append_u32_le(&response, FILE_OPENERS_BINARY_ROW_SIZE) &&
         sb_append_u32_le(&response, FILE_OPENERS_BINARY_HEADER_SIZE) &&
         sb_append_u32_le(&response, (uint32_t)variable_offset) &&
         sb_append_u32_le(&response, (uint32_t)total_size) &&
         sb_append_u32_le(&response, 0) &&
         sb_append_n(&response, rows.data ? rows.data : "", rows.length) &&
         sb_append_n(&response, variable.data ? variable.data : "", variable.length);
    free(rows.data);
    free(variable.data);

    if (!ok) {
        free(response.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_response(fd, 200, "OK", "application/octet-stream", response.data, response.length);
    free(response.data);
}

static void send_openers_response(int fd,
                                  const char *query,
                                  const char *requester_user) {
    char requested[PATH_MAX];
    if (!query_value(query, "path", requested, sizeof(requested))) {
        send_text_response(fd, 400, "missing path\n");
        return;
    }
    send_openers_response_for_path(fd, requested, requester_user);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_size; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            int high = hex_value(src[i + 1]);
            int low = hex_value(src[i + 2]);
            dst[out++] = (char)((high << 4) | low);
            i += 2;
        } else if (src[i] == '+') {
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool query_value(const char *query, const char *name, char *dst, size_t dst_size) {
    if (!query) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t pair_len = end ? (size_t)(end - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', pair_len);
        if (equals && (size_t)(equals - cursor) == name_len && strncmp(cursor, name, name_len) == 0) {
            char encoded[PATH_MAX * 3];
            size_t value_len = pair_len - name_len - 1;
            if (value_len >= sizeof(encoded)) {
                value_len = sizeof(encoded) - 1;
            }
            memcpy(encoded, equals + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(dst, dst_size, encoded);
            return true;
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        return home;
    }
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

static bool mkdir_p(const char *path) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    size_t len = strlen(copy);
    if (len == 0) return false;
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir(copy, 0755) == 0 || errno == EEXIST;
}

static void expand_tilde_path(const char *path, char *out, size_t out_size) {
    if (!path || !path[0]) {
        out[0] = '\0';
    } else if (strcmp(path, "~") == 0) {
        snprintf(out, out_size, "%s", home_directory());
    } else if (path[0] == '~' && path[1] == '/') {
        snprintf(out, out_size, "%s/%s", home_directory(), path + 2);
    } else {
        snprintf(out, out_size, "%s", path);
    }
}

static void default_socket_path(char *out, size_t out_size) {
    const char *label = g_backend_label[0] ? g_backend_label : "org.outershell.Files";
#ifdef __APPLE__
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, label);
    } else {
        snprintf(out, out_size, "%s/Library/%s", home_directory(), label);
    }
#else
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, label);
    } else {
        snprintf(out, out_size, "/run/user/%d/%s", (int)getuid(), label);
    }
#endif
}

static void send_app_announcement_to_outershelld(const char *action, int port, const char *socket_path) {
    if (!g_outershelld_api_socket_path[0] || !g_backend_label[0]) return;
    if (strcmp(action, "add") == 0) {
        (void)send_app_add_to_outershelld(port, socket_path);
    } else if (strcmp(action, "remove") == 0) {
        (void)send_app_remove_to_outershelld(port, socket_path);
    }
}

static void cleanup_handler(void) {
    if (g_listen_socket_path[0] && !g_systemd_socket_activation) {
        send_app_announcement_to_outershelld("remove", 0, g_listen_socket_path);
        if (!g_listen_socket_is_launchd_owned) {
            unlink(g_listen_socket_path);
        }
    }
}

static void resolve_requested_path(const char *requested, char *resolved, size_t resolved_size) {
    const char *home = home_directory();
    if (!requested || requested[0] == '\0' || strcmp(requested, "~") == 0) {
        snprintf(resolved, resolved_size, "%s", home);
    } else if (requested[0] == '~' && requested[1] == '/') {
        snprintf(resolved, resolved_size, "%s/%s", home, requested + 2);
    } else {
        snprintf(resolved, resolved_size, "%s", requested);
    }
}

static void parent_path_for(const char *path, char *parent, size_t parent_size) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) {
        snprintf(parent, parent_size, "/");
        return;
    }
    *slash = '\0';
    snprintf(parent, parent_size, "%s", copy);
}

static bool join_child_path(const char *directory, const char *name, char *out, size_t out_size) {
    int written;
    if (strcmp(directory, "/") == 0) {
        written = snprintf(out, out_size, "/%s", name);
    } else {
        written = snprintf(out, out_size, "%s/%s", directory, name);
    }
    return written >= 0 && (size_t)written < out_size;
}

static void mode_string(mode_t mode, char out[11]) {
    out[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
    const mode_t bits[] = {
        S_IRUSR, S_IWUSR, S_IXUSR,
        S_IRGRP, S_IWGRP, S_IXGRP,
        S_IROTH, S_IWOTH, S_IXOTH
    };
    const char chars[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & bits[i]) ? chars[i] : '-';
    }
    out[10] = '\0';
}

static int compare_entries(const void *lhs, const void *rhs) {
    const FileEntry *a = (const FileEntry *)lhs;
    const FileEntry *b = (const FileEntry *)rhs;
    if (a->is_directory != b->is_directory) {
        return a->is_directory ? -1 : 1;
    }
    return strcasecmp(a->name, b->name);
}

static void requester_access_context_destroy(RequesterAccessContext *context) {
    if (!context) return;
    free(context->groups);
    context->groups = NULL;
    context->group_count = 0;
}

static void requester_access_context_init(RequesterAccessContext *context, const char *requester_user) {
    if (!context) return;
    memset(context, 0, sizeof(*context));

    struct passwd *pw = NULL;
    if (requester_user && requester_user[0]) {
        pw = getpwnam(requester_user);
    }
    if (!pw) {
        pw = getpwuid(getuid());
    }
    if (!pw) {
        return;
    }

    context->resolved = true;
    context->uid = pw->pw_uid;
    context->primary_gid = pw->pw_gid;

#ifdef __APPLE__
    int group_count = 64;
    int stack_groups[64];
    int *groups = stack_groups;
    if (getgrouplist(pw->pw_name, (int)pw->pw_gid, groups, &group_count) < 0) {
        groups = calloc((size_t)group_count, sizeof(int));
        if (!groups) {
            context->group_count = 0;
            return;
        }
        if (getgrouplist(pw->pw_name, (int)pw->pw_gid, groups, &group_count) < 0) {
            free(groups);
            context->group_count = 0;
            return;
        }
    }
    context->groups = calloc((size_t)group_count, sizeof(gid_t));
    if (context->groups) {
        for (int i = 0; i < group_count; i++) {
            context->groups[i] = (gid_t)groups[i];
        }
        context->group_count = group_count;
    }
    if (groups != stack_groups) {
        free(groups);
    }
#else
    int group_count = 64;
    gid_t stack_groups[64];
    gid_t *groups = stack_groups;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &group_count) < 0) {
        groups = calloc((size_t)group_count, sizeof(gid_t));
        if (!groups) {
            context->group_count = 0;
            return;
        }
        if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &group_count) < 0) {
            free(groups);
            context->group_count = 0;
            return;
        }
    }
    context->groups = calloc((size_t)group_count, sizeof(gid_t));
    if (context->groups) {
        memcpy(context->groups, groups, (size_t)group_count * sizeof(gid_t));
        context->group_count = group_count;
    }
    if (groups != stack_groups) {
        free(groups);
    }
#endif
}

static bool requester_access_context_contains_gid(const RequesterAccessContext *context, gid_t gid) {
    if (!context || !context->resolved) return false;
    if (context->primary_gid == gid) return true;
    for (int i = 0; i < context->group_count; i++) {
        if (context->groups[i] == gid) {
            return true;
        }
    }
    return false;
}

static uint32_t requester_access_flags_for_stat(const RequesterAccessContext *context, const struct stat *st) {
    if (!context || !context->resolved || !st) return 0;

    if (context->uid == 0) {
        return FILE_ENTRY_ACCESS_USER_READ | FILE_ENTRY_ACCESS_USER_WRITE;
    }

    mode_t read_bit;
    mode_t write_bit;
    if (context->uid == st->st_uid) {
        read_bit = S_IRUSR;
        write_bit = S_IWUSR;
    } else if (requester_access_context_contains_gid(context, st->st_gid)) {
        read_bit = S_IRGRP;
        write_bit = S_IWGRP;
    } else {
        read_bit = S_IROTH;
        write_bit = S_IWOTH;
    }

    uint32_t flags = 0;
    if ((st->st_mode & read_bit) != 0) flags |= FILE_ENTRY_ACCESS_USER_READ;
    if ((st->st_mode & write_bit) != 0) flags |= FILE_ENTRY_ACCESS_USER_WRITE;
    return flags;
}

static bool append_file_list_entry_row(StringBuilder *rows,
                                       StringBuilder *variable,
                                       const FileEntry *entry) {
    char mode[11];
    mode_string(entry->mode, mode);
    uint64_t modified_millis = entry->modified > 0 ? (uint64_t)(entry->modified * 1000.0) : 0;
    return append_binary_string_ref(rows, variable, entry->name) &&
           append_binary_string_ref(rows, variable, entry->path) &&
           append_binary_string_ref(rows, variable, mode) &&
           sb_append_u32_le(rows, entry->is_directory ? 1u : 0u) &&
           sb_append_u32_le(rows, entry->access_flags) &&
           sb_append_u64_le(rows, entry->size) &&
           sb_append_u64_le(rows, modified_millis);
}

static uint64_t stat_modified_millis(const struct stat *st) {
    if (!st) return 0;
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000u + (uint64_t)st->st_mtimespec.tv_nsec / 1000000u;
#else
    return (uint64_t)st->st_mtim.tv_sec * 1000u + (uint64_t)st->st_mtim.tv_nsec / 1000000u;
#endif
}

static void send_files_response_for_path(int fd, const char *requested, const char *requester_user) {
    char path[PATH_MAX];
    resolve_requested_path(requested, path, sizeof(path));

    DIR *dir = opendir(path);
    if (!dir) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "failed to open %s: %s\n", path, strerror(errno));
        send_text_response(fd, 404, message);
        return;
    }

    RequesterAccessContext access_context;
    requester_access_context_init(&access_context, requester_user);

    FileEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 64;
            FileEntry *new_entries = realloc(entries, new_capacity * sizeof(FileEntry));
            if (!new_entries) {
                free(entries);
                requester_access_context_destroy(&access_context);
                closedir(dir);
                send_text_response(fd, 500, "out of memory\n");
                return;
            }
            entries = new_entries;
            capacity = new_capacity;
        }

        FileEntry *file = &entries[count];
        memset(file, 0, sizeof(*file));
        snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
        if (!join_child_path(path, entry->d_name, file->path, sizeof(file->path))) {
            continue;
        }

        struct stat st;
        if (stat(file->path, &st) != 0 && lstat(file->path, &st) != 0) {
            continue;
        }
        file->is_directory = S_ISDIR(st.st_mode);
        file->size = (uint64_t)st.st_size;
        file->modified = (double)stat_modified_millis(&st) / 1000.0;
        file->mode = st.st_mode;
        file->access_flags = requester_access_flags_for_stat(&access_context, &st);
        count++;
    }
    closedir(dir);
    requester_access_context_destroy(&access_context);

    qsort(entries, count, sizeof(FileEntry), compare_entries);

    char parent[PATH_MAX];
    parent_path_for(path, parent, sizeof(parent));

    StringBuilder header_refs = {0};
    StringBuilder rows = {0};
    StringBuilder variable = {0};
    bool ok = append_binary_string_ref(&header_refs, &variable, path) &&
              append_binary_string_ref(&header_refs, &variable, parent);
    for (size_t i = 0; ok && i < count; i++) {
        ok = append_file_list_entry_row(&rows, &variable, &entries[i]);
    }
    free(entries);

    size_t variable_offset = FILE_LIST_BINARY_HEADER_SIZE + rows.length;
    size_t total_size = variable_offset + variable.length;
    if (ok && (header_refs.length != 16 || rows.length > UINT32_MAX || variable.length > UINT32_MAX ||
               variable_offset > UINT32_MAX || total_size > UINT32_MAX || count > UINT32_MAX)) {
        ok = false;
    }
    if (!ok) {
        free(header_refs.data);
        free(rows.data);
        free(variable.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    patch_string_refs_to_absolute_offsets(header_refs.data, header_refs.length, (uint32_t)variable_offset);
    patch_file_list_row_string_refs(rows.data, rows.length, (uint32_t)variable_offset);

    StringBuilder response = {0};
    ok = sb_append_u32_le(&response, FILE_LIST_BINARY_MAGIC) &&
         sb_append_u32_le(&response, FILE_LIST_BINARY_VERSION) &&
         sb_append_u32_le(&response, (uint32_t)count) &&
         sb_append_u32_le(&response, FILE_LIST_BINARY_ROW_SIZE) &&
         sb_append_u32_le(&response, FILE_LIST_BINARY_HEADER_SIZE) &&
         sb_append_u32_le(&response, (uint32_t)variable_offset) &&
         sb_append_u32_le(&response, (uint32_t)total_size) &&
         sb_append_u32_le(&response, 0) &&
         sb_append_n(&response, header_refs.data, header_refs.length) &&
         sb_append_n(&response, rows.data ? rows.data : "", rows.length) &&
         sb_append_n(&response, variable.data ? variable.data : "", variable.length);
    free(header_refs.data);
    free(rows.data);
    free(variable.data);

    if (!ok) {
        free(response.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_response(fd, 200, "OK", "application/octet-stream", response.data, response.length);
    free(response.data);
}

static void send_files_response(int fd, const char *query, const char *requester_user) {
    char requested[PATH_MAX];
    if (!query_value(query, "path", requested, sizeof(requested))) {
        requested[0] = '\0';
    }
    send_files_response_for_path(fd, requested, requester_user);
}

static void send_preview_response_for_path(int fd, const char *requested) {
    char path[PATH_MAX];
    resolve_requested_path(requested, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_text_response(fd, 404, "file not found\n");
        return;
    }

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        char message[PATH_MAX + 80];
        snprintf(message, sizeof(message), "failed to open %s: %s\n", path, strerror(errno));
        send_text_response(fd, 404, message);
        return;
    }

    const char *content_type = preview_content_type_for_path(path);
    bool is_text_preview = preview_content_type_is_text(content_type);
    size_t max_bytes = is_text_preview ? FILE_TEXT_PREVIEW_MAX_BYTES : FILE_MEDIA_PREVIEW_MAX_BYTES;

    if (!is_text_preview && (uint64_t)st.st_size > (uint64_t)max_bytes) {
        char message[PATH_MAX + 128];
        snprintf(message, sizeof(message), "preview is too large: %s exceeds 16 MiB\n", path);
        close(file_fd);
        send_text_response(fd, 413, message);
        return;
    }

    const char *truncation_notice = "\n\n[Preview truncated at 256 KiB]\n";
    size_t notice_len = strlen(truncation_notice);
    size_t capacity = max_bytes + (is_text_preview ? notice_len : 0);
    char *buffer = malloc(capacity ? capacity : 1);
    if (!buffer) {
        close(file_fd);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    size_t offset = 0;
    while (offset < max_bytes) {
        ssize_t got = read(file_fd, buffer + offset, max_bytes - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            char message[PATH_MAX + 80];
            snprintf(message, sizeof(message), "failed to read %s: %s\n", path, strerror(errno));
            free(buffer);
            close(file_fd);
            send_text_response(fd, 500, message);
            return;
        }
        if (got == 0) {
            break;
        }
        offset += (size_t)got;
    }
    close(file_fd);

    if (is_text_preview && (uint64_t)st.st_size > (uint64_t)offset && offset + notice_len <= capacity) {
        memcpy(buffer + offset, truncation_notice, notice_len);
        offset += notice_len;
    }

    send_response(fd, 200, "OK", content_type, buffer, offset);
    free(buffer);
}

static void send_preview_metadata_response_for_path(int fd, const char *requested) {
    enum {
        FILE_PREVIEW_METADATA_BINARY_MAGIC = 0x534d5046u,
        FILE_PREVIEW_METADATA_BINARY_VERSION = 1u,
        FILE_PREVIEW_METADATA_BINARY_SIZE = 24u
    };

    char path[PATH_MAX];
    resolve_requested_path(requested, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_text_response(fd, 404, "file not found\n");
        return;
    }

    unsigned char payload[FILE_PREVIEW_METADATA_BINARY_SIZE];
    write_uint32_le(payload + 0, FILE_PREVIEW_METADATA_BINARY_MAGIC);
    write_uint32_le(payload + 4, FILE_PREVIEW_METADATA_BINARY_VERSION);
    write_uint64_le(payload + 8, (uint64_t)st.st_size);
    write_uint64_le(payload + 16, stat_modified_millis(&st));
    send_response(fd, 200, "OK", "application/octet-stream", payload, sizeof(payload));
}

static void send_download_response(int fd, const char *query) {
    char requested[PATH_MAX];
    char path[PATH_MAX];
    if (!query_value(query, "path", requested, sizeof(requested))) {
        send_text_response(fd, 400, "missing path\n");
        return;
    }
    resolve_requested_path(requested, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_text_response(fd, 404, "file not found\n");
        return;
    }
    send_bundle_file(fd, path);
}

static bool safe_upload_name(const char *name) {
    if (!name || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            return false;
        }
    }
    return true;
}

static void send_upload_response(int fd, const char *query, const unsigned char *body, size_t body_len) {
    char requested_directory[PATH_MAX];
    char directory[PATH_MAX];
    char name[NAME_MAX + 1];
    char output_path[PATH_MAX];

    if (!query_value(query, "directory", requested_directory, sizeof(requested_directory)) ||
        !query_value(query, "name", name, sizeof(name))) {
        send_text_response(fd, 400, "missing directory or name\n");
        return;
    }
    if (!safe_upload_name(name)) {
        send_text_response(fd, 400, "invalid file name\n");
        return;
    }

    resolve_requested_path(requested_directory, directory, sizeof(directory));
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_text_response(fd, 404, "destination directory not found\n");
        return;
    }
    if (!join_child_path(directory, name, output_path, sizeof(output_path))) {
        send_text_response(fd, 400, "path is too long\n");
        return;
    }

    int file_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file_fd < 0) {
        send_text_response(fd, 500, "failed to open destination\n");
        return;
    }
    bool ok = queue_all(file_fd, body, body_len);
    if (close(file_fd) != 0) {
        ok = false;
    }
    if (!ok) {
        send_text_response(fd, 500, "failed to write destination\n");
        return;
    }
    send_text_response(fd, 200, "ok\n");
}

static void send_mkdir_response_for_inputs(int fd, const char *requested_directory, const char *name) {
    char directory[PATH_MAX];
    char output_path[PATH_MAX];

    if (!safe_upload_name(name)) {
        send_text_response(fd, 400, "invalid directory name\n");
        return;
    }

    resolve_requested_path(requested_directory, directory, sizeof(directory));
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_text_response(fd, 404, "destination directory not found\n");
        return;
    }
    if (!join_child_path(directory, name, output_path, sizeof(output_path))) {
        send_text_response(fd, 400, "path is too long\n");
        return;
    }

    if (mkdir(output_path, 0755) != 0) {
        if (errno == EEXIST && stat(output_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            send_text_response(fd, 200, "ok\n");
            return;
        }
        send_text_response(fd, 500, "failed to create directory\n");
        return;
    }
    send_text_response(fd, 200, "ok\n");
}

static void send_mkdir_response(int fd, const char *query) {
    char requested_directory[PATH_MAX];
    char name[NAME_MAX + 1];

    if (!query_value(query, "directory", requested_directory, sizeof(requested_directory)) ||
        !query_value(query, "name", name, sizeof(name))) {
        send_text_response(fd, 400, "missing directory or name\n");
        return;
    }
    send_mkdir_response_for_inputs(fd, requested_directory, name);
}

static size_t request_content_length(const char *request) {
    const char *line = request;
    while ((line = strcasestr(line, "\r\nContent-Length:")) != NULL) {
        line += strlen("\r\nContent-Length:");
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        return (size_t)strtoull(line, NULL, 10);
    }
    return 0;
}

static bool request_header_value(const char *request, const char *name, char *dst, size_t dst_size) {
    if (!request || !name || !name[0] || !dst || dst_size == 0) return false;
    dst[0] = '\0';
    size_t name_len = strlen(name);
    const char *headers_end = strstr(request, "\r\n\r\n");
    if (!headers_end) return false;
    const char *line = strstr(request, "\r\n");
    if (!line || line >= headers_end) return false;
    line += 2;
    while (line < headers_end && *line) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > headers_end) line_end = headers_end;
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && (size_t)(colon - line) == name_len && strncasecmp(line, name, name_len) == 0) {
            const char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            const char *trimmed_end = line_end;
            while (trimmed_end > value && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) trimmed_end--;
            size_t len = (size_t)(trimmed_end - value);
            if (len >= dst_size) len = dst_size - 1;
            memcpy(dst, value, len);
            dst[len] = '\0';
            return true;
        }
        line = line_end + 2;
    }
    return false;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static void handle_http_request(int fd,
                                char *request,
                                size_t request_header_length,
                                unsigned char *body,
                                size_t content_length) {
    set_blocking(fd);

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char method[16], target[1024], version[16];
    if (sscanf(request, "%15s %1023s %15s", method, target, version) != 3) {
        free(body);
        send_text_response(fd, 400, "bad request\n");
        return;
    }
    if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "HEAD") != 0 && strcasecmp(method, "POST") != 0) {
        free(body);
        send_text_response(fd, 400, "unsupported method\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    char requester_user[256];
    request_header_value(request, "X-Files-User", requester_user, sizeof(requester_user));

    if (strcasecmp(method, "POST") == 0) {
        if (strcmp(target, "/api/files") == 0) {
            char requested[PATH_MAX];
            if (!read_binary_path_request(body, content_length, FILE_PATH_REQUEST_BINARY_MAGIC, requested, sizeof(requested))) {
                free(body);
                send_text_response(fd, 400, "bad files request\n");
                return;
            }
            send_files_response_for_path(fd, requested, requester_user);
            free(body);
            return;
        }
        if (strcmp(target, "/api/openers") == 0) {
            char requested[PATH_MAX];
            if (!read_binary_path_request(body, content_length, FILE_OPENERS_REQUEST_BINARY_MAGIC, requested, sizeof(requested))) {
                free(body);
                send_text_response(fd, 400, "bad openers request\n");
                return;
            }
            send_openers_response_for_path(fd, requested, requester_user);
            free(body);
            return;
        }
        if (strcmp(target, "/api/preview") == 0) {
            char requested[PATH_MAX];
            if (!read_binary_path_request(body, content_length, FILE_PATH_REQUEST_BINARY_MAGIC, requested, sizeof(requested))) {
                free(body);
                send_text_response(fd, 400, "bad preview request\n");
                return;
            }
            send_preview_response_for_path(fd, requested);
            free(body);
            return;
        }
        if (strcmp(target, "/api/preview-metadata") == 0) {
            char requested[PATH_MAX];
            if (!read_binary_path_request(body, content_length, FILE_PATH_REQUEST_BINARY_MAGIC, requested, sizeof(requested))) {
                free(body);
                send_text_response(fd, 400, "bad preview metadata request\n");
                return;
            }
            send_preview_metadata_response_for_path(fd, requested);
            free(body);
            return;
        }
        if (strcmp(target, "/api/mkdir") == 0) {
            char requested_directory[PATH_MAX];
            char name[NAME_MAX + 1];
            if (read_binary_directory_name_request(body, content_length, FILE_MKDIR_REQUEST_BINARY_MAGIC,
                                                   requested_directory, sizeof(requested_directory),
                                                   name, sizeof(name))) {
                send_mkdir_response_for_inputs(fd, requested_directory, name);
            } else {
                send_mkdir_response(fd, query);
            }
            free(body);
            return;
        }
        if (strcmp(target, "/api/upload") != 0) {
            free(body);
            send_text_response(fd, 404, "not found\n");
            return;
        }
        send_upload_response(fd, query, body, content_length);
        free(body);
    } else if (strcmp(target, "/") == 0 && !request_accepts_outerframe(request, request_header_length)) {
        free(body);
        send_web_file(fd, "index.html", "text/html; charset=utf-8", request, request_header_length,
                      strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, "/web/app.css") == 0) {
        free(body);
        send_web_file(fd, "app.css", "text/css; charset=utf-8", request, request_header_length,
                      strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, "/web/app.js") == 0) {
        free(body);
        send_web_file(fd, "app.js", "text/javascript; charset=utf-8", request, request_header_length,
                      strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, "/web/folder-icon.png") == 0) {
        free(body);
        send_web_file(fd, "folder-icon.png", "image/png", request, request_header_length,
                      strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, "/") == 0 || strcmp(target, "/files.outer") == 0) {
        free(body);
        send_outer_descriptor(fd, request, request_header_length,
                              strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, kBundleUrlPath) == 0) {
        free(body);
        send_text_response(fd, 200, "macos-arm\nmacos-x86\n");
    } else if (strcmp(target, kBundleUrlPathMacosArm) == 0) {
        free(body);
        const char *path = g_bundle_file_path_macos_arm[0] ? g_bundle_file_path_macos_arm : kBundleFilePathMacosArm;
        send_cached_bundle_file(fd, path, request, request_header_length,
                                strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, kBundleUrlPathMacosX86) == 0) {
        free(body);
        const char *path = g_bundle_file_path_macos_x86[0] ? g_bundle_file_path_macos_x86 : kBundleFilePathMacosX86;
        send_cached_bundle_file(fd, path, request, request_header_length,
                                strcasecmp(method, "HEAD") != 0);
    } else if (strcmp(target, "/api/files") == 0) {
        free(body);
        send_files_response(fd, query, requester_user);
    } else if (strcmp(target, "/api/openers") == 0) {
        free(body);
        send_openers_response(fd, query, requester_user);
    } else if (strcmp(target, "/api/preview") == 0) {
        char requested[PATH_MAX] = "";
        query_value(query ? query : "", "path", requested, sizeof(requested));
        free(body);
        send_preview_response_for_path(fd, requested);
    } else if (strcmp(target, "/api/preview-metadata") == 0) {
        char requested[PATH_MAX] = "";
        query_value(query ? query : "", "path", requested, sizeof(requested));
        free(body);
        send_preview_metadata_response_for_path(fd, requested);
    } else if (strcmp(target, "/api/download") == 0) {
        free(body);
        send_download_response(fd, query);
    } else {
        free(body);
        send_text_response(fd, 404, "not found\n");
    }
}

static void close_http_client(HttpClient *client) {
    if (!client) return;
    if (client->fd >= 0) {
        close(client->fd);
    }
    free(client->body);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static bool http_client_has_complete_request(HttpClient *client) {
    return client->header_len > 0 && client->body && client->body_len >= client->content_length;
}

static bool read_http_client_available(HttpClient *client) {
    while (true) {
        if (client->header_len == 0) {
            if (client->request_len + 1 >= sizeof(client->request)) {
                send_text_response(client->fd, 400, "request headers too large\n");
                return false;
            }
            ssize_t got = read(client->fd,
                               client->request + client->request_len,
                               sizeof(client->request) - client->request_len - 1);
            if (got < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                return false;
            }
            if (got == 0) return false;
            client->request_len += (size_t)got;
            client->request[client->request_len] = '\0';

            char *headers_end = strstr(client->request, "\r\n\r\n");
            if (!headers_end) {
                continue;
            }

            const char *initial_body = headers_end + 4;
            client->header_len = (size_t)(initial_body - client->request);
            client->content_length = request_content_length(client->request);
            size_t initial_body_len = client->request_len > client->header_len ? client->request_len - client->header_len : 0;
            if (initial_body_len > client->content_length) initial_body_len = client->content_length;
            client->body = malloc(client->content_length ? client->content_length : 1);
            if (!client->body) {
                send_text_response(client->fd, 500, "out of memory\n");
                return false;
            }
            memcpy(client->body, initial_body, initial_body_len);
            client->body_len = initial_body_len;
            client->request[client->header_len] = '\0';
            if (http_client_has_complete_request(client)) return true;
        }

        if (client->body_len < client->content_length) {
            ssize_t got = read(client->fd,
                               client->body + client->body_len,
                               client->content_length - client->body_len);
            if (got < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                return false;
            }
            if (got == 0) return false;
            client->body_len += (size_t)got;
            if (http_client_has_complete_request(client)) return true;
            continue;
        }

        return true;
    }
}

static void compact_http_clients(HttpClient *clients, size_t *client_count) {
    size_t write_index = 0;
    for (size_t read_index = 0; read_index < *client_count; read_index++) {
        if (clients[read_index].fd < 0) {
            continue;
        }
        if (write_index != read_index) {
            clients[write_index] = clients[read_index];
        }
        write_index++;
    }
    *client_count = write_index;
}

static void run_server_loop(int listener) {
    enum { MAX_HTTP_CLIENTS = 128, IDLE_CLIENT_TIMEOUT_SECONDS = 15 };
    HttpClient clients[MAX_HTTP_CLIENTS];
    for (size_t i = 0; i < MAX_HTTP_CLIENTS; i++) {
        clients[i].fd = -1;
    }
    size_t client_count = 0;
    set_nonblocking(listener);

    while (!g_shutdown_requested) {
        struct pollfd poll_fds[MAX_HTTP_CLIENTS + 1];
        poll_fds[0].fd = listener;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;
        for (size_t i = 0; i < client_count; i++) {
            poll_fds[i + 1].fd = clients[i].fd;
            poll_fds[i + 1].events = POLLIN;
            poll_fds[i + 1].revents = 0;
        }

        int timeout_ms = g_systemd_socket_activation && client_count == 0 ? 60000 : 1000;
        int poll_result = poll(poll_fds, (nfds_t)(client_count + 1), timeout_ms);
        if (poll_result == 0) {
            if (g_systemd_socket_activation && client_count == 0) {
                break;
            }
        } else if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        size_t polled_client_count = client_count;
        if (poll_result > 0 && (poll_fds[0].revents & POLLIN)) {
            while (client_count < MAX_HTTP_CLIENTS) {
                struct sockaddr_storage peer;
                socklen_t peer_len = sizeof(peer);
                int client_fd = accept(listener, (struct sockaddr *)&peer, &peer_len);
                if (client_fd < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    perror("accept");
                    g_shutdown_requested = 1;
                    break;
                }
                set_nonblocking(client_fd);
                HttpClient *client = &clients[client_count++];
                memset(client, 0, sizeof(*client));
                client->fd = client_fd;
                client->accepted_at = time(NULL);
            }
        }

        for (size_t i = 0; i < polled_client_count; i++) {
            HttpClient *client = &clients[i];
            short revents = poll_fds[i + 1].revents;
            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close_http_client(client);
                continue;
            }
            if ((revents & POLLIN) && !read_http_client_available(client)) {
                close_http_client(client);
                continue;
            }
            if (http_client_has_complete_request(client)) {
                handle_http_request(client->fd, client->request, client->header_len,
                                    client->body, client->content_length);
                client->body = NULL;
                close_http_client(client);
                continue;
            }
            if (client->header_len == 0 && time(NULL) - client->accepted_at > IDLE_CLIENT_TIMEOUT_SECONDS) {
                close_http_client(client);
            }
        }
        compact_http_clients(clients, &client_count);
    }

    for (size_t i = 0; i < client_count; i++) {
        close_http_client(&clients[i]);
    }
}

static int create_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static int create_unix_listener(const char *socket_path) {
    if (!socket_path || !socket_path[0]) {
        fprintf(stderr, "socket path is required\n");
        return -1;
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "socket path is too long: %s\n", socket_path);
        return -1;
    }

    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", socket_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(directory)) {
            fprintf(stderr, "failed to create socket directory %s: %s\n", directory, strerror(errno));
            return -1;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    unlink(socket_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (chmod(socket_path, 0600) != 0) {
        perror("chmod");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        perror("listen");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    return fd;
}

static int systemd_activated_listener(void) {
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (!listen_pid || !listen_fds) {
        return -1;
    }
    char *end = NULL;
    long pid = strtol(listen_pid, &end, 10);
    if (!end || *end != '\0' || pid != (long)getpid()) {
        return -1;
    }
    end = NULL;
    long fds = strtol(listen_fds, &end, 10);
    if (!end || *end != '\0' || fds < 1) {
        return -1;
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    g_systemd_socket_activation = true;
    return 3;
}

#ifdef __APPLE__
static int create_launchd_unix_listener(const char *socket_name, const char *socket_path) {
    int *fds = NULL;
    size_t count = 0;
    int result = launch_activate_socket(socket_name, &fds, &count);
    if (result != 0) {
        errno = result;
        perror("launch_activate_socket");
        return -1;
    }
    if (!fds || count == 0) {
        fprintf(stderr, "launchd socket unavailable\n");
        free(fds);
        return -1;
    }

    int listen_fd = fds[0];
    for (size_t i = 1; i < count; i++) {
        close(fds[i]);
    }
    free(fds);

    if (socket_path && socket_path[0]) {
        snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    } else {
        default_socket_path(g_listen_socket_path, sizeof(g_listen_socket_path));
    }
    g_listen_socket_is_launchd_owned = true;
    return listen_fd;
}
#endif

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--launchd-socket-name NAME] [--api-socket-path PATH] [--label LABEL] [--bundles-dir DIR] [--web-root DIR] [--icon-file PATH]\n", program);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    bool use_port = false;
    char socket_path[PATH_MAX] = "";
    const char *bundles_dir = NULL;
#ifdef __APPLE__
    char launchd_socket_name[128] = "";
#endif
    default_outershelld_api_socket_path(g_outershelld_api_socket_path, sizeof(g_outershelld_api_socket_path));
#ifdef __APPLE__
    configure_resource_paths_from_app_bundle();
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            use_port = true;
            socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], socket_path, sizeof(socket_path));
            use_port = false;
#ifdef __APPLE__
        } else if (strcmp(argv[i], "--launchd-socket-name") == 0 && i + 1 < argc) {
            snprintf(launchd_socket_name, sizeof(launchd_socket_name), "%s", argv[++i]);
            use_port = false;
#endif
        } else if (strcmp(argv[i], "--api-socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_outershelld_api_socket_path, sizeof(g_outershelld_api_socket_path));
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            snprintf(g_backend_label, sizeof(g_backend_label), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            bundles_dir = argv[++i];
            snprintf(g_web_root, sizeof(g_web_root), "%s/../web", bundles_dir);
        } else if (strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) {
            snprintf(g_web_root, sizeof(g_web_root), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--icon-file") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_app_icon_path, sizeof(g_app_icon_path));
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!use_port && !socket_path[0]) {
        default_socket_path(socket_path, sizeof(socket_path));
    }

    if (bundles_dir) {
        snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
                 "%s/FilesContent.bundle.macos-arm.aar", bundles_dir);
        snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
                 "%s/FilesContent.bundle.macos-x86.aar", bundles_dir);
    } else if (!g_bundle_file_path_macos_arm[0] || !g_bundle_file_path_macos_x86[0]) {
        snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
                 "%s", kBundleFilePathMacosArm);
        snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
                 "%s", kBundleFilePathMacosX86);
    }

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup_handler);

    int listener = !use_port ? systemd_activated_listener() : -1;
    if (listener < 0) {
        if (use_port) {
            listener = create_tcp_listener(port);
#ifdef __APPLE__
        } else if (launchd_socket_name[0]) {
            listener = create_launchd_unix_listener(launchd_socket_name, socket_path);
#endif
        } else {
            listener = create_unix_listener(socket_path);
        }
    } else if (socket_path[0]) {
        snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    }
    if (listener < 0) {
        return 1;
    }
    g_listener_fd = listener;
    if (use_port) {
        fprintf(stderr, "FilesBackend listening on http://127.0.0.1:%d/\n", port);
        send_app_announcement_to_outershelld("add", port, NULL);
    } else {
        fprintf(stderr, "FilesBackend listening on %s/\n", socket_path);
        send_app_announcement_to_outershelld("add", 0, socket_path);
    }

    run_server_loop(listener);

    close(listener);
    g_listener_fd = -1;
    return 0;
}
