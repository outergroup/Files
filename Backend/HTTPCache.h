#ifndef OUTER_HTTP_CACHE_H
#define OUTER_HTTP_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static const char *outer_http_cache_line_end(const char *cursor, const char *end) {
    while (cursor < end && *cursor != '\0') {
        if (cursor + 1 < end && cursor[0] == '\r' && cursor[1] == '\n') return cursor;
        cursor++;
    }
    return cursor;
}

static bool outer_http_cache_request_header(const char *request,
                                            size_t header_length,
                                            const char *header_name,
                                            const char **value_out,
                                            size_t *value_length_out) {
    if (!request || !header_name || !value_out || !value_length_out) return false;
    const char *cursor = request;
    const char *end = request + header_length;
    const char *line_end = outer_http_cache_line_end(cursor, end);
    if (line_end >= end || *line_end == '\0') return false;
    cursor = line_end + 2;

    size_t name_length = strlen(header_name);
    while (cursor < end && *cursor != '\0') {
        line_end = outer_http_cache_line_end(cursor, end);
        if (line_end == cursor) break;
        const char *colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon && (size_t)(colon - cursor) == name_length &&
            strncasecmp(cursor, header_name, name_length) == 0) {
            const char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            const char *value_end = line_end;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) value_end--;
            *value_out = value;
            *value_length_out = (size_t)(value_end - value);
            return true;
        }
        if (line_end >= end || *line_end == '\0') break;
        cursor = line_end + 2;
    }
    return false;
}

static long outer_http_cache_mtime_nanoseconds(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#else
    return st->st_mtim.tv_nsec;
#endif
}

static long outer_http_cache_ctime_nanoseconds(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_ctimespec.tv_nsec;
#else
    return st->st_ctim.tv_nsec;
#endif
}

static void outer_http_cache_file_etag(const struct stat *st, char *out, size_t out_size) {
    snprintf(out, out_size, "W/\"%llx-%lx-%llx-%lx-%llx\"",
             (unsigned long long)st->st_mtime,
             (unsigned long)outer_http_cache_mtime_nanoseconds(st),
             (unsigned long long)st->st_ctime,
             (unsigned long)outer_http_cache_ctime_nanoseconds(st),
             (unsigned long long)st->st_size);
}

static void outer_http_cache_memory_etag(const void *body,
                                         size_t body_length,
                                         char *out,
                                         size_t out_size) {
    const unsigned char *bytes = body;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < body_length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    snprintf(out, out_size, "W/\"%016llx-%zx\"",
             (unsigned long long)hash, body_length);
}

static void outer_http_cache_format_date(time_t value, char *out, size_t out_size) {
    static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm date;
    if (!gmtime_r(&value, &date) || date.tm_wday < 0 || date.tm_wday > 6 ||
        date.tm_mon < 0 || date.tm_mon > 11) {
        snprintf(out, out_size, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }
    snprintf(out, out_size, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             weekdays[date.tm_wday], date.tm_mday, months[date.tm_mon],
             date.tm_year + 1900, date.tm_hour, date.tm_min, date.tm_sec);
}

static bool outer_http_cache_parse_date(const char *value, size_t value_length, time_t *out) {
    if (value_length == 0 || value_length >= 128) return false;
    char text[128];
    memcpy(text, value, value_length);
    text[value_length] = '\0';
    static const char *formats[] = {
        "%a, %d %b %Y %H:%M:%S GMT",
        "%A, %d-%b-%y %H:%M:%S GMT",
        "%a %b %e %H:%M:%S %Y"
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct tm date;
        memset(&date, 0, sizeof(date));
        char *end = strptime(text, formats[i], &date);
        if (end && *end == '\0') {
            date.tm_isdst = 0;
            time_t timestamp = timegm(&date);
            struct tm normalized;
            if (gmtime_r(&timestamp, &normalized) &&
                normalized.tm_year == date.tm_year &&
                normalized.tm_mon == date.tm_mon &&
                normalized.tm_mday == date.tm_mday &&
                normalized.tm_hour == date.tm_hour &&
                normalized.tm_min == date.tm_min &&
                normalized.tm_sec == date.tm_sec) {
                *out = timestamp;
                return true;
            }
        }
    }
    return false;
}

static void outer_http_cache_trim(const char **value, size_t *length) {
    while (*length > 0 && (**value == ' ' || **value == '\t')) {
        (*value)++;
        (*length)--;
    }
    while (*length > 0 && ((*value)[*length - 1] == ' ' || (*value)[*length - 1] == '\t')) {
        (*length)--;
    }
}

static bool outer_http_cache_weak_etag_equal(const char *candidate,
                                             size_t candidate_length,
                                             const char *etag) {
    outer_http_cache_trim(&candidate, &candidate_length);
    if (candidate_length >= 2 && candidate[0] == 'W' && candidate[1] == '/') {
        candidate += 2;
        candidate_length -= 2;
    }
    size_t etag_length = strlen(etag);
    if (etag_length >= 2 && etag[0] == 'W' && etag[1] == '/') {
        etag += 2;
        etag_length -= 2;
    }
    return candidate_length == etag_length && memcmp(candidate, etag, etag_length) == 0;
}

static bool outer_http_cache_if_none_match(const char *value,
                                           size_t value_length,
                                           const char *etag) {
    const char *cursor = value;
    const char *end = value + value_length;
    while (cursor < end) {
        while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == ',')) cursor++;
        if (cursor >= end) break;
        if (*cursor == '*') {
            const char *after = cursor + 1;
            while (after < end && (*after == ' ' || *after == '\t')) after++;
            if (after == end || *after == ',') return true;
        }
        const char *candidate = cursor;
        bool quoted = false;
        while (cursor < end) {
            if (*cursor == '"') quoted = !quoted;
            if (*cursor == ',' && !quoted) break;
            cursor++;
        }
        if (outer_http_cache_weak_etag_equal(candidate,
                                             (size_t)(cursor - candidate),
                                             etag)) return true;
        if (cursor < end) cursor++;
    }
    return false;
}

static bool outer_http_cache_not_modified(const char *request,
                                          size_t header_length,
                                          const char *etag,
                                          const time_t *last_modified) {
    const char *condition = NULL;
    size_t condition_length = 0;
    if (outer_http_cache_request_header(request, header_length, "If-None-Match",
                                        &condition, &condition_length)) {
        return outer_http_cache_if_none_match(condition, condition_length, etag);
    }
    if (last_modified &&
        outer_http_cache_request_header(request, header_length, "If-Modified-Since",
                                        &condition, &condition_length)) {
        time_t modified_since;
        return outer_http_cache_parse_date(condition, condition_length, &modified_since) &&
               *last_modified <= modified_since;
    }
    return false;
}

static size_t outer_http_cache_response_header(char *out,
                                               size_t out_size,
                                               int status,
                                               const char *content_type,
                                               size_t content_length,
                                               const char *etag,
                                               const char *last_modified) {
    int length;
    if (status == 304) {
        length = snprintf(out, out_size,
                          "HTTP/1.1 304 Not Modified\r\n"
                          "Connection: close\r\n"
                          "Cache-Control: public, max-age=0, must-revalidate\r\n"
                          "ETag: %s\r\n"
                          "Last-Modified: %s\r\n"
                          "\r\n",
                          etag, last_modified);
    } else {
        length = snprintf(out, out_size,
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "Cache-Control: public, max-age=0, must-revalidate\r\n"
                          "ETag: %s\r\n"
                          "Last-Modified: %s\r\n"
                          "\r\n",
                          content_type, content_length, etag, last_modified);
    }
    return length > 0 && (size_t)length < out_size ? (size_t)length : 0;
}

static time_t outer_http_cache_server_start_time(void) {
    static time_t start_time = 0;
    if (start_time == 0) start_time = time(NULL);
    return start_time;
}

#endif

