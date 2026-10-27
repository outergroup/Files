#ifndef ENVIRONMENT_TOOLS_H
#define ENVIRONMENT_TOOLS_H
#include <sys/wait.h>

static bool environment_request_header(const char *request, size_t length) {
    const char *key = "x-environment-request: 1";
    size_t size = strlen(key);
    for (size_t i = 0; i + size <= length; i++) {
        if ((i == 0 || request[i - 1] == '\n') && !strncasecmp(request + i, key, size) &&
            (i + size == length || request[i + size] == '\r' || request[i + size] == '\n')) return true;
    }
    return false;
}

#include "Environment.h"
static char *environment_tools_request(const char *method, const void *body, size_t length, size_t *result_length) {
    if (strcmp(method, "POST")) return NULL;
    return environment_request(body, length, result_length);
}
#endif
