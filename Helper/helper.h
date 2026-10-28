#ifndef ENVIRONMENT_HELPER_H
#define ENVIRONMENT_HELPER_H
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#define MAX_FRAME 65536u
#define MAX_DATA (16u * 1024u * 1024u)
#define MAX_TEXT (4u * 1024u * 1024u)
#define HELPER_NAME "outer-files-helper"
#define HELPER_PATH "/usr/local/bin/" HELPER_NAME
#define HELPER_CAPABILITIES 515u
typedef struct {
    unsigned char *data;
    size_t size;
} Bytes;
typedef struct {
    pid_t pid;
    int input, output, error;
} Child;
#include "binary.h"
extern _Thread_local char failure[1024];
extern double deadline;
void fail(const char *format, ...);
void *allocate(size_t size);
char *format(const char *fmt, ...);
bool utf8(const unsigned char *data, size_t size);
bool append(Bytes *bytes, const void *data, size_t size, size_t limit);
void bytes_free(Bytes *bytes);
Bytes read_file(const char *path, size_t limit, bool regular);
bool write_all(int fd, const void *data, size_t size);
bool read_all(int fd, void *data, size_t size, bool *eof);
double now(void);
bool expand_path(const char *path, char result[PATH_MAX]);
bool spawn(char *const *args, Child *child);
void child_close(Child *child);
bool frame_write(int fd, uint32_t kind, uint32_t id, const void *data, size_t size);
bool frame_read(int fd, uint32_t *kind, uint32_t *id, Bytes *body, bool *eof);
bool message_write(int fd, uint32_t id, Bytes metadata, Bytes body);
bool message_read(int fd, uint32_t id, BinaryReply *reply, bool *eof);
int serve(void);
#endif
