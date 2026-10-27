#include "environment.h"
#include <glob.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "OuterShellPaths.h"

static void target_add(EnvironmentSnapshot *snapshot, EnvironmentTarget target) {
    EnvironmentTarget *next = realloc(snapshot->targets, (snapshot->target_count + 1) * sizeof(*next));
    if (!next)
        exit(1);
    snapshot->targets = next;
    snapshot->targets[snapshot->target_count++] = target;
}
static void error_add(EnvironmentSnapshot *snapshot, const char *runtime, const char *message) {
    EnvironmentError *next = realloc(snapshot->errors, (snapshot->error_count + 1) * sizeof(*next));
    if (!next)
        exit(1);
    snapshot->errors = next;
    snapshot->errors[snapshot->error_count++] = (EnvironmentError){strdup(runtime), strdup(message)};
}
void environment_snapshot_free(EnvironmentSnapshot *snapshot) {
    for (size_t i = 0; i < snapshot->target_count; i++) {
        EnvironmentTarget *t = &snapshot->targets[i];
        free(t->id);
        free(t->runtime);
        free(t->container);
        free(t->name);
        free(t->state);
        free(t->image);
        free(t->user);
        free(t->executable);
        free(t->socket);
    }
    for (size_t i = 0; i < snapshot->error_count; i++) {
        free(snapshot->errors[i].runtime);
        free(snapshot->errors[i].message);
    }
    free(snapshot->targets);
    free(snapshot->errors);
    *snapshot = (EnvironmentSnapshot){0};
}

static char *find_executable(const char *name) {
    const char *path = getenv("PATH");
    char *dirs = format("%s:/opt/homebrew/bin:/usr/local/bin:%s/.local/bin", path ? path : "/usr/bin:/bin",
                        getenv("HOME") ? getenv("HOME") : "");
    char *save = NULL, *result = NULL;
    for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = format("%s/%s", dir, name);
        struct stat st;
        if (!stat(candidate, &st) && S_ISREG(st.st_mode) && !access(candidate, X_OK)) {
            result = candidate;
            break;
        }
        free(candidate);
    }
    free(dirs);
    return result;
}
static bool valid_identifier(const char *s, size_t limit, bool user) {
    size_t n = strlen(s);
    if (!n || n > limit || !(isalnum((unsigned char)s[0]) || (user && s[0] == '_')))
        return false;
    for (size_t i = 1; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr(user ? "_.:-" : "_.-", s[i]))
            return false;
    return true;
}
static Bytes registry_query(const char *path) {
    Bytes response = {0};
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) {
        fail("registry socket path is too long");
        return response;
    }
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("registry socket: %s", strerror(errno));
        return response;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        fail("Outer Shell registry unavailable: %s", strerror(errno));
        close(fd);
        return response;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    const unsigned char request[] = {2, 0, 0, 0, 34, 0};
    unsigned char header[4];
    bool eof;
    if (write_all(fd, request, sizeof(request)) && read_all(fd, header, sizeof(header), &eof)) {
        size_t size = get_u32(header);
        if (size < 22 || size > MAX_DATA)
            fail("invalid registry response size");
        else {
            response = (Bytes){allocate(size), size};
            if (!read_all(fd, response.data, size, &eof))
                bytes_free(&response);
        }
    }
    close(fd);
    return response;
}
static bool registry_targets(EnvironmentSnapshot *snapshot, Bytes bytes, uid_t uid) {
    if (bytes.size >= 22 && bytes.data[0] == 100 && bytes.data[1] == 0) {
        char *error = binary_read_string(bytes, 14, 22, MAX_DATA);
        if (error) {
            fail("%s", error);
            free(error);
        }
        return false;
    }
    if (bytes.size < 48 || memcmp(bytes.data, "OSED", 4) || get_u32(bytes.data + 4) != 1 ||
        get_u32(bytes.data + 8) != bytes.size || get_u32(bytes.data + 16) != 72 ||
        get_u32(bytes.data + 20) != 48 || get_u32(bytes.data + 28) > 1 || get_u64(bytes.data + 40)) {
        fail("invalid registry environment header; update Outer Shell");
        return false;
    }
    size_t count = get_u32(bytes.data + 12), variable = get_u32(bytes.data + 24);
    if (variable > bytes.size || variable != 48 + (uint64_t)count * 72) {
        fail("invalid registry environment records");
        return false;
    }
    char *error = binary_read_string(bytes, 32, variable, 4096);
    if (!error)
        return false;
    if (get_u32(bytes.data + 28))
        fail("%s", *error ? error : "registry query failed");
    free(error);
    for (size_t i = 0; i < count && !failure[0]; i++) {
        char *f[9] = {0};
        for (size_t j = 0; j < 9; j++)
            f[j] = binary_read_string(bytes, 48 + i * 72 + j * 8, variable, 4096);
        if (!failure[0]) {
            const char *kind = !strcmp(f[2], "apple.container") ? "apple" : f[2];
            bool docker = !strcmp(kind, "docker");
            bool supported = docker || !strcmp(kind, "apple") || !strcmp(kind, "wslc");
            if (!valid_identifier(f[0], 256, false) || (*f[3] && !valid_identifier(f[3], 256, false)))
                fail("invalid registered container identity");
            else if (!supported)
                error_add(snapshot, f[1], "Unsupported registered container runtime");
            else {
                char *rootless = format("unix:///run/user/%u/docker.sock", (unsigned)uid);
                char *key =
                    docker && !strcmp(f[6], rootless) && uid != 0
                        ? (geteuid() == 0 ? format("docker-user-%u", (unsigned)uid) : strdup("docker-user"))
                        : strdup(kind);
                free(rootless);
                char *exe = find_executable(docker                   ? "docker"
                                            : !strcmp(kind, "apple") ? "container"
                                                                     : "wslc");
                if (geteuid() == 0 && uid != 0) {
                    char *expected = format("unix:///run/user/%u/docker.sock", (unsigned)uid);
                    if (!docker || strcmp(f[6], expected)) {
                        error_add(snapshot, f[1],
                                  "Cross-user execution requires that user's rootless Docker socket");
                        free(exe);
                        exe = NULL;
                    }
                    free(expected);
                }
                EnvironmentTarget t = {.id = *f[3] ? format("%s:%s", key, f[3])
                                                   : format("registered-%u:%s", (unsigned)uid, f[0]),
                                       .runtime = key,
                                       .container = strdup(f[3]),
                                       .name = strdup(f[1]),
                                       .state = strdup(exe ? f[4] : "unavailable"),
                                       .image = strdup(f[5]),
                                       .user = strdup(""),
                                       .executable = exe,
                                       .socket = strdup(docker ? f[6] : "")};
                if (*f[7])
                    error_add(snapshot, t.name, f[7]);
                if (!exe)
                    error_add(snapshot, t.name, "Container runtime executable is unavailable");
                bool duplicate = false;
                for (size_t j = 0; j < snapshot->target_count; j++)
                    if (!strcmp(snapshot->targets[j].id, t.id))
                        duplicate = true;
                if (!duplicate)
                    target_add(snapshot, t);
                else {
                    EnvironmentSnapshot discarded = {0};
                    target_add(&discarded, t);
                    environment_snapshot_free(&discarded);
                }
            }
        }
        for (size_t j = 0; j < 9; j++)
            free(f[j]);
    }
    return !failure[0];
}
static void discover_registry(EnvironmentSnapshot *result, const char *path, uid_t uid) {
    failure[0] = 0;
    Bytes bytes = registry_query(path);
    EnvironmentSnapshot part = {0};
    if (bytes.data && registry_targets(&part, bytes, uid)) {
        for (size_t i = 0; i < part.target_count; i++) {
            target_add(result, part.targets[i]);
            part.targets[i] = (EnvironmentTarget){0};
        }
        for (size_t i = 0; i < part.error_count; i++)
            error_add(result, part.errors[i].runtime, part.errors[i].message);
    } else
        error_add(result, "Outer Shell", failure[0] ? failure : "Registry query failed");
    environment_snapshot_free(&part);
    bytes_free(&bytes);
    failure[0] = 0;
}
static void sessions_prune(const EnvironmentSnapshot *snapshot);
EnvironmentSnapshot discover_environments(void) {
    EnvironmentSnapshot result = {0};
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)))
        strcpy(hostname, "Host");
    hostname[sizeof(hostname) - 1] = 0;
    struct passwd *pw = getpwuid(geteuid());
    char uid[32];
    snprintf(uid, sizeof(uid), "%u", (unsigned)geteuid());
    target_add(&result, (EnvironmentTarget){.id = strdup("host"),
                                            .runtime = strdup("host"),
                                            .container = strdup(""),
                                            .name = strdup(hostname),
                                            .state = strdup("running"),
                                            .image = strdup(""),
                                            .user = strdup(pw ? pw->pw_name : uid)});
    char path[PATH_MAX];
    outer_shell_default_api_socket_path(path, sizeof(path));
    discover_registry(&result, path, geteuid());
#ifdef __linux__
    char system_path[PATH_MAX];
    outer_shell_default_system_api_socket_path(system_path, sizeof(system_path));
    if (geteuid() == 0 && !strcmp(path, system_path)) {
        glob_t paths = {0};
        if (!glob("/run/user/*/outershelld-api", 0, NULL, &paths))
            for (size_t i = 0; i < paths.gl_pathc; i++) {
                struct stat info;
                unsigned owner;
                if (!lstat(paths.gl_pathv[i], &info) && S_ISSOCK(info.st_mode) && info.st_uid != 0 &&
                    sscanf(paths.gl_pathv[i], "/run/user/%u/outershelld-api", &owner) == 1 &&
                    owner == info.st_uid)
                    discover_registry(&result, paths.gl_pathv[i], info.st_uid);
            }
        globfree(&paths);
    }
#endif
    sessions_prune(&result);
    return result;
}
typedef struct {
    char *key, *target_id;
    Child child;
    uint32_t next_id;
    double used;
} HelperSession;
static HelperSession sessions[32];
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
static void session_close(HelperSession *session) {
    if (!session->key) return;
    child_close(&session->child);
    free(session->key); free(session->target_id);
    *session = (HelperSession){0};
}
void sessions_close(void) {
    for (size_t i = 0; i < 32; i++) {
        if (sessions[i].key && sessions[i].child.input >= 0) { close(sessions[i].child.input); sessions[i].child.input = -1; }
    }
    for (size_t i = 0; i < 32; i++) session_close(&sessions[i]);
}
static void sessions_prune(const EnvironmentSnapshot *snapshot) {
    for (size_t i = 0; i < 32; i++) {
        HelperSession *s = &sessions[i];
        if (!s->key) continue;
        bool registered = false;
        for (size_t j = 1; j < snapshot->target_count; j++)
            if (!strcmp(s->target_id, snapshot->targets[j].id) &&
                !strcmp(snapshot->targets[j].state, "running")) registered = true;
        if (!registered) session_close(s);
    }
}
static bool session_alive(HelperSession *session) {
    struct pollfd p = {session->child.output, POLLIN, 0};
    return session->child.pid > 0 && poll(&p, 1, 0) == 0;
}
static BinaryReply target_operation(const EnvironmentTarget *target, const char *user, const Request *request,
                                    Bytes body) {
    BinaryReply reply = {0};
    if (strcmp(target->state, "running") || !target->executable || !*target->container) {
        fail("The registered container is not running or its runtime is unavailable."); return reply;
    }
    if (*user && !valid_identifier(user, 128, true)) { fail("invalid container user"); return reply; }
    char *key = format("%zu:%s%zu:%s%zu:%s%zu:%s", strlen(target->executable), target->executable,
                       strlen(target->socket ? target->socket : ""), target->socket ? target->socket : "",
                       strlen(target->container), target->container, strlen(user), user);
    pthread_mutex_lock(&session_mutex);
    HelperSession *session = NULL;
    for (size_t i = 0; i < 32; i++)
        if (sessions[i].key && !strcmp(sessions[i].key, key)) { session = &sessions[i]; break; }
    if (session && (!session_alive(session) || !session->next_id)) session_close(session);
    if (!session) {
        for (size_t i = 0; i < 32; i++) if (!sessions[i].key) { session = &sessions[i]; break; }
        if (!session) {
            session = &sessions[0];
            for (size_t i = 1; i < 32; i++) if (sessions[i].used < session->used) session = &sessions[i];
            session_close(session);
        }
    }
    bool safe_retry = request->operation == OP_LIST || request->operation == OP_READ || request->operation == OP_PROCESSES;
    for (int attempt = 0; attempt < (safe_retry ? 2 : 1); attempt++) {
        if (!session->key) {
            char *args[16]; size_t n = 0;
            args[n++] = target->executable;
            if (target->socket && *target->socket) { args[n++] = "--host"; args[n++] = target->socket; }
            args[n++] = "exec"; args[n++] = "-i";
            if (*user) { args[n++] = "--user"; args[n++] = (char *)user; }
            args[n++] = target->container; args[n++] = HELPER_PATH; args[n++] = "serve"; args[n] = NULL;
            if (!spawn(args, &session->child)) break;
            session->key = strdup(key); session->target_id = strdup(target->id); session->next_id = 1;
            uint32_t kind = 0, id = 0; Bytes hello = {0}; bool eof;
            bool ok = frame_read(session->child.output, &kind, &id, &hello, &eof) && kind == 1 && id == 0 &&
                      hello.size == 16 && get_u32(hello.data) == 2 && get_u32(hello.data + 4) == HELPER_CAPABILITIES &&
                      get_u32(hello.data + 8) == MAX_DATA && get_u32(hello.data + 12) == MAX_FRAME;
            bytes_free(&hello);
            if (!ok) {
                fail("Container helper unavailable or incompatible; install " HELPER_NAME " in the Dockerfile.");
                session_close(session); break;
            }
        }
        Request typed = *request; typed.data_length = body.size;
        Bytes metadata = request_encode(&typed); bool eof;
        uint32_t id = session->next_id++;
        bool received = metadata.data && message_write(session->child.input, id, metadata, body) &&
                        message_read(session->child.output, id, &reply, &eof);
        bytes_free(&metadata);
        if (received) {
            bool valid = response_validate(reply, typed.operation);
            if (!valid && (reply.metadata.size < RESPONSE_SIZE || get_u32(reply.metadata.data + 12) != 1))
                session_close(session);
            if (!valid) binary_reply_free(&reply);
            session->used = now();
            break;
        }
        binary_reply_free(&reply);
        session_close(session);
        if (!safe_retry) {
            fail("Container connection lost. The operation may have completed; check its result before retrying."); break;
        }
        if (!attempt) failure[0] = 0;
    }
    pthread_mutex_unlock(&session_mutex);
    free(key);
    return reply;
}
BinaryReply environment_binary_operation(const char *target, const char *user, const Request *request,
                                         Bytes body) {
    if (!strcmp(target, "host")) {
        BinaryReply reply = local_binary_operation(request, body);
        response_finish(&reply);
        return reply;
    }
    if (*user && !valid_identifier(user, 128, true)) {
        fail("invalid container user");
        return (BinaryReply){0};
    }
    const char *colon = strchr(target, ':');
    if (!colon || !valid_identifier(colon + 1, 256, false)) {
        fail("invalid environment");
        return (BinaryReply){0};
    }
    EnvironmentSnapshot snapshot = discover_environments();
    BinaryReply reply = {0};
    bool found = false;
    for (size_t i = 1; i < snapshot.target_count; i++) {
        if (!strcmp(snapshot.targets[i].id, target)) {
            found = true;
            reply = target_operation(&snapshot.targets[i], user, request, body);
            break;
        }
    }
    if (!found)
        fail("The container is no longer registered or was replaced. Refresh the container selection.");
    environment_snapshot_free(&snapshot);
    return reply;
}
