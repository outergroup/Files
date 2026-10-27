#include "helper.h"
_Thread_local char failure[1024];
double deadline;
void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(failure, sizeof(failure), fmt, args);
    va_end(args);
}
void *allocate(size_t size) {
    void *p = calloc(1, size ? size : 1);
    if (!p) {
        fputs("Out of memory\n", stderr);
        exit(1);
    }
    return p;
}
char *format(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *s = NULL;
    int n = vasprintf(&s, fmt, args);
    va_end(args);
    if (n < 0) {
        fputs("Out of memory\n", stderr);
        exit(1);
    }
    return s;
}
void bytes_free(Bytes *b) {
    free(b->data);
    *b = (Bytes){0};
}
bool append(Bytes *b, const void *p, size_t n, size_t max) {
    if (n > max || b->size > max - n) {
        fail("size limit exceeded");
        return false;
    }
    unsigned char *q = realloc(b->data, b->size + n + 1);
    if (!q) {
        fail("out of memory");
        return false;
    }
    b->data = q;
    if (n)
        memcpy(q + b->size, p, n);
    b->size += n;
    q[b->size] = 0;
    return true;
}
bool utf8(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = p[i++];
        if (!c)
            return false;
        if (c < 128)
            continue;
        unsigned v;
        size_t extra;
        if (c >= 0xc2 && c <= 0xdf) {
            v = c & 31;
            extra = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            v = c & 15;
            extra = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            v = c & 7;
            extra = 3;
        } else
            return false;
        if (extra > n - i)
            return false;
        for (size_t k = 0; k < extra; k++) {
            c = p[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            v = (v << 6) | (c & 63);
        }
        if ((extra == 2 && v < 0x800) || (extra == 3 && v < 0x10000) || v > 0x10ffff ||
            (v >= 0xd800 && v <= 0xdfff))
            return false;
    }
    return true;
}
double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static bool wait_fd(int fd, short events) {
    for (;;) {
        double left = deadline ? deadline - now() : 3600;
        if (left <= 0) {
            fail("operation timed out");
            return false;
        }
        struct pollfd p = {fd, events, 0};
        int r = poll(&p, 1, (int)(left > 3600 ? 3600000 : left * 1000 + 1));
        if (r > 0)
            return true;
        if ((r < 0 && errno == EINTR) || (r == 0 && !deadline))
            continue;
        fail(r == 0 ? "operation timed out" : "poll failed: %s", strerror(errno));
        return false;
    }
}
bool write_all(int fd, const void *p, size_t n) {
    while (n) {
        if (!wait_fd(fd, POLLOUT))
            return false;
        ssize_t k = write(fd, p, n);
        if (k < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (k <= 0) {
            fail("write: %s", strerror(errno));
            return false;
        }
        p = (const char *)p + k;
        n -= (size_t)k;
    }
    return true;
}
bool read_all(int fd, void *p, size_t n, bool *eof) {
    size_t done = 0;
    *eof = false;
    while (done < n) {
        if (!wait_fd(fd, POLLIN))
            return false;
        ssize_t k = read(fd, (char *)p + done, n - done);
        if (k < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (k <= 0) {
            *eof = k == 0 && done == 0;
            fail(k == 0 ? "truncated stream" : "read: %s", strerror(errno));
            return false;
        }
        done += (size_t)k;
    }
    return true;
}
Bytes read_file(const char *path, size_t limit, bool regular) {
    Bytes b = {0};
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fail("%s: %s", path, strerror(errno));
        return b;
    }
    struct stat st;
    if (fstat(fd, &st)) {
        int error = errno;
        close(fd);
        errno = error;
        fail("stat %s: %s", path, strerror(error));
        return b;
    }
    if (regular && !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        fail("not a regular file: %s", path);
        return b;
    }
    if (st.st_size > (off_t)limit) {
        close(fd);
        errno = EFBIG;
        fail("file exceeds size limit");
        return b;
    }
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            fail("read %s: %s", path, strerror(errno));
            break;
        }
        if (!n)
            break;
        if (!append(&b, buf, (size_t)n, limit))
            break;
    }
    close(fd);
    if (failure[0])
        bytes_free(&b);
    else if (!b.data)
        append(&b, "", 0, limit);
    return b;
}
bool expand_path(const char *input, char out[PATH_MAX]) {
    char tmp[PATH_MAX];
    if (!input || !*input)
        input = "~";
    if (strlen(input) >= sizeof(tmp)) {
        fail("path is too long");
        return false;
    }
    if (input[0] == '~' && (input[1] == '/' || !input[1])) {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw) {
            fail("cannot resolve current user's home");
            return false;
        }
        if (snprintf(tmp, sizeof(tmp), "%s%s", pw->pw_dir, input + 1) >= (int)sizeof(tmp)) {
            fail("path is too long");
            return false;
        }
    } else
        snprintf(tmp, sizeof(tmp), "%s", input);
    if (tmp[0] != '/') {
        fail("path must be absolute or start with ~");
        return false;
    }
    out[0] = '/';
    out[1] = 0;
    char *save = NULL;
    for (char *p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            char *slash = strrchr(out, '/');
            if (slash == out)
                out[1] = 0;
            else
                *slash = 0;
            continue;
        }
        size_t n = strlen(out);
        if (n > 1)
            strcat(out, "/");
        strcat(out, p);
    }
    return true;
}
