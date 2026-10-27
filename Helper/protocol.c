#include "helper.h"
static void put32(unsigned char *p, uint32_t n) { put_u32(p, n); }
static uint32_t get32(unsigned char *p) { return get_u32(p); }
bool frame_write(int fd, uint32_t kind, uint32_t id, const void *data, size_t size) {
    if (size > MAX_FRAME) {
        fail("frame exceeds limit");
        return false;
    }
    unsigned char h[16];
    memcpy(h, "FLH2", 4);
    put32(h + 4, kind);
    put32(h + 8, id);
    put32(h + 12, (uint32_t)size);
    return write_all(fd, h, 16) && write_all(fd, data, size);
}
bool frame_read(int fd, uint32_t *kind, uint32_t *id, Bytes *body, bool *eof) {
    unsigned char h[16];
    if (!read_all(fd, h, 16, eof))
        return false;
    if (memcmp(h, "FLH2", 4)) {
        fail("invalid helper protocol magic");
        return false;
    }
    *kind = get32(h + 4);
    *id = get32(h + 8);
    size_t n = get32(h + 12);
    if (n > MAX_FRAME) {
        fail("frame exceeds limit");
        return false;
    }
    body->data = allocate(n + 1);
    body->size = n;
    bool end;
    if (!read_all(fd, body->data, n, &end)) {
        *eof = false;
        bytes_free(body);
        return false;
    }
    return true;
}
static bool chunks(int fd, uint32_t kind, uint32_t id, const void *p, size_t n) {
    while (n) {
        size_t k = n > MAX_FRAME ? MAX_FRAME : n;
        if (!frame_write(fd, kind, id, p, k))
            return false;
        p = (const char *)p + k;
        n -= k;
    }
    return true;
}
bool message_write(int fd, uint32_t id, Bytes metadata, Bytes body) {
    bool ok = metadata.size <= MAX_DATA && body.size <= MAX_DATA;
    if (!ok) {
        fail("message exceeds limit");
        return false;
    }
    return chunks(fd, 2, id, metadata.data, metadata.size) && chunks(fd, 3, id, body.data, body.size) &&
           frame_write(fd, 4, id, NULL, 0);
}
bool message_read(int fd, uint32_t id, BinaryReply *reply, bool *eof) {
    bool started = false, body_started = false, ok = false;
    *eof = false;
    for (;;) {
        Bytes data = {0};
        uint32_t kind, got;
        bool end = false;
        if (!frame_read(fd, &kind, &got, &data, &end)) {
            *eof = end && !started;
            break;
        }
        started = true;
        if (got != id) {
            fail("helper request ID mismatch");
            bytes_free(&data);
            break;
        }
        if (kind == 2 && !body_started) {
            ok = append(&reply->metadata, data.data, data.size, MAX_DATA);
        } else if (kind == 3) {
            body_started = true;
            ok = append(&reply->body, data.data, data.size, MAX_DATA);
        } else if (kind == 4 && data.size == 0 && reply->metadata.size) {
            bytes_free(&data);
            return true;
        } else {
            ok = false;
            fail("unexpected helper frame order or terminator");
        }
        bytes_free(&data);
        if (!ok)
            break;
    }
    binary_reply_free(reply);
    return false;
}
int serve(void) {
    unsigned char hello[16] = {0};
    put_u32(hello, 2);
    put_u32(hello + 4, HELPER_CAPABILITIES);
    put_u32(hello + 8, MAX_DATA);
    put_u32(hello + 12, MAX_FRAME);
    if (!frame_write(STDOUT_FILENO, 1, 0, hello, sizeof(hello)))
        return 1;
    for (uint32_t id = 1; id; id++) {
        BinaryReply input = {0}, reply = {0};
        Request request = {0};
        bool eof;
        failure[0] = 0;
        if (!message_read(STDIN_FILENO, id, &input, &eof)) {
            if (eof)
                return 0;
            return 1;
        }
        bool ok = request_decode(input.metadata, &request);
        if (ok && request.data_length != input.body.size) {
            fail("request body length mismatch");
            ok = false;
        }
        if (ok)
            reply = local_binary_operation(&request, input.body);
        if (failure[0]) {
            binary_reply_free(&reply);
            reply = response_error(request.operation);
        }
        response_finish(&reply);
        ok = message_write(STDOUT_FILENO, id, reply.metadata, reply.body);
        binary_reply_free(&reply);
        binary_reply_free(&input);
        request_free(&request);
        if (!ok)
            return 1;
    }
    fail("request ID space exhausted");
    return 1;
}
