#include "environment.h"
static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "text/plain";
    struct {
        const char *ext, *type;
    } types[] = {{".png", "image/png"},         {".jpg", "image/jpeg"},      {".jpeg", "image/jpeg"},
                 {".gif", "image/gif"},         {".webp", "image/webp"},     {".svg", "image/svg+xml"},
                 {".ico", "image/x-icon"},      {".pdf", "application/pdf"}, {".mp4", "video/mp4"},
                 {".webm", "video/webm"},       {".mov", "video/quicktime"}, {".mp3", "audio/mpeg"},
                 {".wav", "audio/wav"},         {".ogg", "audio/ogg"},       {".flac", "audio/flac"},
                 {".json", "application/json"}, {".html", "text/html"},      {".css", "text/css"},
                 {".js", "text/javascript"},    {".xml", "text/xml"}};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(ext, types[i].ext))
            return types[i].type;
    return "text/plain";
}
enum { CLIENT_HEADER = 72, CLIENT_RESPONSE = 48,
       CLIENT_TARGETS = 7, CLIENT_PREVIEW = 8 };
static Bytes client_response(BinaryReply reply, Bytes snapshot, uint32_t flags, const char *type) {
    char message[sizeof(failure)];
    snprintf(message, sizeof(message), "%s", failure);
    Bytes result = {allocate(CLIENT_RESPONSE), CLIENT_RESPONSE};
    put_u32(result.data, 0x52434c46);
    put_u32(result.data + 4, 1);
    put_u32(result.data + 12, message[0] ? 1 : 0);
    put_u32(result.data + 40, flags);
    failure[0] = 0;
    binary_string(&result, 32, message[0] ? message : type);
    Bytes metadata = snapshot.data ? snapshot : reply.metadata;
    if (message[0]) metadata = (Bytes){0};
    put_u32(result.data + 16, (uint32_t)result.size);
    put_u32(result.data + 20, (uint32_t)metadata.size);
    append(&result, metadata.data, metadata.size, MAX_DATA * 2);
    put_u32(result.data + 24, (uint32_t)result.size);
    put_u32(result.data + 28, message[0] ? 0 : (uint32_t)reply.body.size);
    if (!message[0]) append(&result, reply.body.data, reply.body.size, MAX_DATA * 2);
    put_u32(result.data + 8, (uint32_t)result.size);
    if (failure[0]) bytes_free(&result);
    return result;
}
Bytes dispatch_binary(Bytes input, const char *method) {
    Request request = {0};
    char *target = NULL, *user = NULL;
    BinaryReply reply = {0};
    Bytes snapshot = {0}, body = {0};
    uint32_t operation = 0, flags = 0;
    const char *type = "";
    if (failure[0]) goto done;
    if (strcmp(method, "POST")) { fail("environment requests require POST"); goto done; }
    if (input.size < CLIENT_HEADER || get_u32(input.data) != 0x51434c46 ||
        get_u32(input.data + 4) != 1 || get_u32(input.data + 8) != input.size ||
        get_u64(input.data + 16)) { fail("invalid environment request header"); goto done; }
    operation = get_u32(input.data + 12);
    bool allowed = operation == CLIENT_TARGETS || operation == OP_LIST || operation == OP_PLACES || operation == OP_ADD_PLACE || operation == CLIENT_PREVIEW;
    if (!allowed) { fail("operation is not supported by this app"); goto done; }
    target = binary_read_string(input, 24, CLIENT_HEADER, 4096);
    user = binary_read_string(input, 32, CLIENT_HEADER, 256);
    request.path = binary_read_string(input, 40, CLIENT_HEADER, PATH_MAX - 1);
    request.destination = binary_read_string(input, 48, CLIENT_HEADER, PATH_MAX - 1);
    request.revision = binary_read_string(input, 56, CLIENT_HEADER, 64);
    request.start = strdup("");
    if (!target || !user || !request.path || !request.destination || !request.revision) goto done;
    char *fields[] = {target, user, request.path, request.destination, request.revision};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (!utf8((unsigned char *)fields[i], strlen(fields[i]))) { fail("invalid UTF-8 request"); goto done; }
    size_t offset = get_u32(input.data + 64), length = get_u32(input.data + 68);
    if (offset < CLIENT_HEADER || offset > input.size || length > input.size - offset || length > MAX_TEXT ||
        length) {
        fail("invalid request body"); goto done;
    }
    body = (Bytes){input.data + offset, length};
    if (!utf8(body.data, body.size)) { fail("document is not UTF-8 text"); goto done; }
    if (operation == CLIENT_TARGETS) {
        const EnvironmentSnapshot *environments = environment_registry(true);
        snapshot = environment_snapshot_encode(environments);
        goto done;
    }
    if (operation == OP_ADD_PLACE && strcmp(target, "host")) {
        fail("Adding favorites is currently supported only on the host"); goto done;
    }
    request.operation = operation == CLIENT_PREVIEW ? OP_READ : operation;
    request.limit = operation == CLIENT_PREVIEW ? MAX_DATA : MAX_TEXT;
    request.data_length = body.size;
    reply = environment_binary_operation(target, user, &request, body);
    if (failure[0] || !response_validate(reply, request.operation)) goto done;
    if (operation == CLIENT_PREVIEW) { type = mime_type(request.path); goto done; }

done:
    response_finish(&reply);
    Bytes result = client_response(reply, snapshot, flags, type);
    free(target); free(user); request_free(&request);
    binary_reply_free(&reply); bytes_free(&snapshot);
    return result;
}

Bytes environment_operation(Bytes input) { return dispatch_binary(input, "POST"); }
