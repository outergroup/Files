#include "environment.h"
enum { SNAPSHOT_HEADER = 80, TARGET_SIZE = 56, SNAPSHOT_PROCESS_SIZE = 112, ERROR_SIZE = 16 };
Bytes environment_snapshot_encode(const EnvironmentSnapshot *snapshot) {
    char message[sizeof(failure)];
    snprintf(message, sizeof(message), "%s", failure);
    size_t targets = message[0] ? 0 : snapshot->target_count;
    size_t processes = 0;
    size_t errors = message[0] ? 0 : snapshot->error_count;
    if (targets > MAX_DATA / TARGET_SIZE || processes > MAX_DATA / SNAPSHOT_PROCESS_SIZE ||
        errors > MAX_DATA / ERROR_SIZE) {
        fail("too many snapshot records");
        return (Bytes){0};
    }
    size_t process_offset = SNAPSHOT_HEADER + targets * TARGET_SIZE;
    size_t error_offset = process_offset + processes * SNAPSHOT_PROCESS_SIZE;
    size_t variable = error_offset + errors * ERROR_SIZE;
    if (variable > MAX_DATA) {
        fail("snapshot exceeds size limit");
        return (Bytes){0};
    }
    Bytes bytes = {allocate(variable), variable};
    put_u32(bytes.data, 0x5054454f);
    put_u32(bytes.data + 4, 1);
    put_u32(bytes.data + 12, message[0] ? 1 : 0);
    put_u32(bytes.data + 16, (uint32_t)targets);
    put_u32(bytes.data + 20, SNAPSHOT_HEADER);
    put_u32(bytes.data + 24, TARGET_SIZE);
    put_u32(bytes.data + 28, (uint32_t)processes);
    put_u32(bytes.data + 32, (uint32_t)process_offset);
    put_u32(bytes.data + 36, SNAPSHOT_PROCESS_SIZE);
    put_u32(bytes.data + 40, (uint32_t)errors);
    put_u32(bytes.data + 44, (uint32_t)error_offset);
    put_u32(bytes.data + 48, ERROR_SIZE);
    put_u32(bytes.data + 52, (uint32_t)variable);
    put_u64(bytes.data + 56, snapshot->time_ms);
    put_u32(bytes.data + 64, (uint32_t)sysconf(_SC_CLK_TCK));
    failure[0] = 0;
    binary_string(&bytes, 72, message);
    for (size_t i = 0; i < targets && !failure[0]; i++) {
        const EnvironmentTarget *t = &snapshot->targets[i];
        const char *fields[] = {t->id, t->runtime, t->container, t->name, t->state, t->image, t->user};
        for (size_t j = 0; j < 7; j++)
            binary_string(&bytes, SNAPSHOT_HEADER + i * TARGET_SIZE + j * 8, fields[j]);
    }
    for (size_t i = 0; i < errors && !failure[0]; i++) {
        binary_string(&bytes, error_offset + i * ERROR_SIZE, snapshot->errors[i].runtime);
        binary_string(&bytes, error_offset + i * ERROR_SIZE + 8, snapshot->errors[i].message);
    }
    if (failure[0]) {
        bytes_free(&bytes);
        return bytes;
    }
    put_u32(bytes.data + 8, (uint32_t)bytes.size);
    return bytes;
}
