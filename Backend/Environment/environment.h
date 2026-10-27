#ifndef ENVIRONMENT_SNAPSHOT_H
#define ENVIRONMENT_SNAPSHOT_H
#include "../../Helper/helper.h"
typedef struct {
    char *id, *runtime, *container, *name, *state, *image, *user;
    char *executable, *socket;
} EnvironmentTarget;
typedef struct {
    char *runtime, *message;
} EnvironmentError;
typedef struct {
    EnvironmentTarget *targets;
    size_t target_count;
    EnvironmentError *errors;
    size_t error_count;
    uint64_t time_ms;
} EnvironmentSnapshot;
EnvironmentSnapshot discover_environments(void);
void environment_snapshot_free(EnvironmentSnapshot *snapshot);
Bytes environment_snapshot_encode(const EnvironmentSnapshot *snapshot);
BinaryReply environment_binary_operation(const char *target, const char *user, const Request *request,
                                         Bytes body);
Bytes environment_operation(Bytes input);
Bytes dispatch_binary(Bytes input, const char *method);
void sessions_close(void);
#endif
