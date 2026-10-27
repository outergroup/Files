#include "helper.h"
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts(HELPER_NAME " 1"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "serve")) return serve();
    fputs("Usage: " HELPER_NAME " serve | --version\n", stderr);
    return 2;
}
