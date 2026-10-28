#ifndef OUTER_APP_RUNTIME_COMMAND_H
#define OUTER_APP_RUNTIME_COMMAND_H
#include <stddef.h>
#include <string.h>

typedef enum {
    APP_RUNTIME_UNKNOWN,
    APP_RUNTIME_DOCKER,
    APP_RUNTIME_APPLE,
    APP_RUNTIME_WSLC
} AppRuntime;

static AppRuntime app_runtime(const char *provider) {
    if (!strcmp(provider, "docker") || !strncmp(provider, "docker-user-", 12) || !strcmp(provider,"docker-user"))
        return APP_RUNTIME_DOCKER;
    if (!strcmp(provider, "apple.container") || !strcmp(provider, "apple") || !strncmp(provider,"apple-user-",11))
        return APP_RUNTIME_APPLE;
    if (!strcmp(provider, "wslc")) return APP_RUNTIME_WSLC;
    return APP_RUNTIME_UNKNOWN;
}

static const char *app_runtime_executable(AppRuntime runtime, int windows) {
    switch (runtime) {
        case APP_RUNTIME_DOCKER: return windows ? "docker.exe" : "docker";
        case APP_RUNTIME_APPLE: return windows ? NULL : "container";
        case APP_RUNTIME_WSLC: return windows ? "wslc.exe" : "wslc";
        default: return NULL;
    }
}

static size_t app_runtime_exec(AppRuntime runtime, const char *executable,
                               const char *endpoint, const char *user,
                               const char *container, const char *helper,
                               char **args, size_t capacity) {
    if (capacity < 12 || runtime == APP_RUNTIME_UNKNOWN || !executable || !*executable ||
        !container || !*container || !helper || !*helper ||
        (runtime != APP_RUNTIME_DOCKER && endpoint && *endpoint)) return 0;
    size_t n = 0;
    args[n++] = (char *)executable;
    if (endpoint && *endpoint) { args[n++] = "--host"; args[n++] = (char *)endpoint; }
    args[n++] = "exec";
    args[n++] = runtime == APP_RUNTIME_WSLC ? "--interactive" : "-i";
    if (user && *user) { args[n++] = "--user"; args[n++] = (char *)user; }
    args[n++] = (char *)container;
    args[n++] = (char *)helper;
    args[n++] = "serve";
    args[n] = NULL;
    return n;
}
#endif
