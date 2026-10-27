#include "Environment.h"
#include "../Helper/common.c"
#include "../Helper/binary.c"
#include "../Helper/child.c"
#include "../Helper/files.c"
#include "../Helper/protocol.c"
#include "Environment/runtime.inc"
#include "Environment/snapshot.inc"
#include "Environment/client.inc"

static pthread_mutex_t environment_mutex = PTHREAD_MUTEX_INITIALIZER;
char *environment_request(const void *body, size_t length, size_t *result_length) {
    if (length > MAX_DATA) return NULL;
    pthread_mutex_lock(&environment_mutex);
    static bool initialized;
    if (!initialized) { atexit(sessions_close); initialized = true; }
    deadline = now() + 25;
    failure[0] = 0;
    Bytes result = environment_operation((Bytes){(unsigned char *)body, length});
    if (result.data) *result_length = result.size;
    pthread_mutex_unlock(&environment_mutex);
    return (char *)result.data;
}
