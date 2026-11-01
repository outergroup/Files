#define APP_FILES 1
#define HELPER_CAPABILITIES 515u
#define HELPER_APP_ID "org.outershell.Files"
#define HELPER_NAME "outer-files-helper"
#define APP_FRAME_MAGIC "FLH2"
#include "AppEnvironment.h"
#include "../Helper/binary.c"
#include "AppEnvironmentFiles.inc"
#include "AppEnvironmentRuntime.inc"
#include "../Backend/Environment/snapshot.inc"
#include "../Backend/Environment/client.inc"
char *environment_request(const void *body, size_t length,
                          size_t *result_length) {
    if (length > MAX_DATA)
        return NULL;
    AcquireSRWLockExclusive(&environment_lock);
    failure[0] = 0;
    deadline = now() + 25;
    Bytes result = {0};
    if (environment_initialize())
        result = environment_operation((Bytes){(unsigned char *)body, length});
    *result_length = result.size;
    ReleaseSRWLockExclusive(&environment_lock);
    return (char *)result.data;
}
