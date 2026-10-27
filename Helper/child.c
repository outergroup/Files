#ifdef __linux__
#include <sys/syscall.h>
#endif
#include "helper.h"
static void close_fd(int *fd) {
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}
bool spawn(char *const *args, Child *child) {
    *child = (Child){.pid = -1, .input = -1, .output = -1, .error = -1};
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    char temp[] = "/tmp/outer-files-helper-stderr-XXXXXX";
    int error = mkstemp(temp);
    if (error >= 0) {
        unlink(temp);
        fcntl(error, F_SETFD, FD_CLOEXEC);
    }
    if (error < 0 || pipe(in) || pipe(out)) {
        fail("create process pipes: %s", strerror(errno));
        goto bad;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(in[i], F_SETFD, FD_CLOEXEC);
        fcntl(out[i], F_SETFD, FD_CLOEXEC);
    }
    long descriptor_limit = sysconf(_SC_OPEN_MAX);
    if (descriptor_limit < 0) descriptor_limit = 65536;
    pid_t pid = fork();
    if (pid < 0) {
        fail("fork: %s", strerror(errno));
        goto bad;
    }
    if (!pid) {
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(error, 2);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        close(error);
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (int fd = 3; fd < descriptor_limit; fd++) close(fd);
        execv(args[0], args);
        _exit(127);
    }
    setpgid(pid, pid);
    close(in[0]);
    close(out[1]);
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    *child = (Child){pid, in[1], out[0], error};
    return true;
bad:
    for (int i = 0; i < 2; i++) {
        close_fd(&in[i]);
        close_fd(&out[i]);
    }
    if (error >= 0)
        close(error);
    return false;
}
void child_close(Child *c) {
    close_fd(&c->input);
    close_fd(&c->output);
    if (c->pid > 0) {
        for (int i = 0; i < 50; i++) {
            pid_t result = waitpid(c->pid, NULL, WNOHANG);
            if (result == c->pid || (result < 0 && errno == ECHILD)) { c->pid = -1; break; }
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
        if (c->pid > 0) kill(-c->pid, SIGKILL);
        while (c->pid > 0 && waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {
        }
        c->pid = -1;
    }
    close_fd(&c->error);
}
