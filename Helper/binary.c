#include "helper.h"
uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get_u64(const unsigned char *p) { return get_u32(p) | ((uint64_t)get_u32(p + 4) << 32); }
void put_u32(unsigned char *p, uint32_t n) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
void put_u64(unsigned char *p, uint64_t n) {
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
bool binary_string(Bytes *b, size_t field, const char *s) {
    if (!s)
        s = "";
    size_t n = strlen(s), offset = b->size;
    if (field > b->size || b->size - field < 8 || offset > UINT32_MAX || n > UINT32_MAX) {
        fail("invalid string reference");
        return false;
    }
    if (!append(b, s, n, MAX_DATA))
        return false;
    put_u32(b->data + field, (uint32_t)offset);
    put_u32(b->data + field + 4, (uint32_t)n);
    return true;
}
char *binary_read_string(Bytes b, size_t field, size_t variable, size_t maximum) {
    if (field > b.size || b.size - field < 8) {
        fail("truncated string reference");
        return NULL;
    }
    size_t offset = get_u32(b.data + field), n = get_u32(b.data + field + 4);
    if (offset < variable || offset > b.size || n > b.size - offset || n > maximum ||
        memchr(b.data + offset, 0, n)) {
        fail("invalid string offset or length");
        return NULL;
    }
    char *s = allocate(n + 1);
    memcpy(s, b.data + offset, n);
    return s;
}
Bytes request_encode(const Request *r) {
    Bytes b = {allocate(REQUEST_SIZE), REQUEST_SIZE};
    put_u32(b.data, REQUEST_MAGIC);
    put_u32(b.data + 4, 2);
    put_u32(b.data + 8, r->operation);
    put_u64(b.data + 16, r->data_length);
    put_u32(b.data + 24, r->pid);
    put_u32(b.data + 28, r->signal);
    put_u64(b.data + 32, r->limit);
    bool ok = binary_string(&b, 40, r->path) && binary_string(&b, 48, r->destination) &&
              binary_string(&b, 56, r->revision) && binary_string(&b, 64, r->start);
    if (!ok) {
        bytes_free(&b);
        return b;
    }
    put_u32(b.data + 12, (uint32_t)b.size);
    return b;
}
bool request_decode(Bytes b, Request *r) {
    if (b.size < REQUEST_SIZE || get_u32(b.data) != REQUEST_MAGIC || get_u32(b.data + 4) != 2 ||
        get_u32(b.data + 12) != b.size || get_u64(b.data + 72)) {
        fail("invalid request header");
        return false;
    }
    r->operation = get_u32(b.data + 8);
    r->data_length = get_u64(b.data + 16);
    r->pid = get_u32(b.data + 24);
    r->signal = get_u32(b.data + 28);
    r->limit = get_u64(b.data + 32);
    if (r->operation < OP_LIST || r->operation > OP_SIGNAL ||
        !(HELPER_CAPABILITIES & (1u << (r->operation - 1))) || r->data_length > MAX_DATA) {
        fail("invalid request operation or body size");
        return false;
    }
    r->path = binary_read_string(b, 40, REQUEST_SIZE, PATH_MAX - 1);
    r->destination = binary_read_string(b, 48, REQUEST_SIZE, PATH_MAX - 1);
    r->revision = binary_read_string(b, 56, REQUEST_SIZE, 64);
    r->start = binary_read_string(b, 64, REQUEST_SIZE, 128);
    return r->path && r->destination && r->revision && r->start;
}
void request_free(Request *r) {
    free(r->path);
    free(r->destination);
    free(r->revision);
    free(r->start);
    *r = (Request){0};
}
BinaryReply response_create(uint32_t op, uint32_t count, uint32_t row_size) {
    BinaryReply r = {0};
    if (row_size && count > (MAX_DATA - RESPONSE_SIZE) / row_size) {
        fail("too many response records");
        return r;
    }
    size_t size = RESPONSE_SIZE + (size_t)count * row_size;
    r.metadata = (Bytes){allocate(size), size};
    put_u32(r.metadata.data, RESPONSE_MAGIC);
    put_u32(r.metadata.data + 4, 2);
    put_u32(r.metadata.data + 8, op);
    put_u32(r.metadata.data + 20, count);
    put_u32(r.metadata.data + 24, row_size);
    put_u32(r.metadata.data + 28, RESPONSE_SIZE);
    put_u32(r.metadata.data + 32, (uint32_t)size);
    binary_string(&r.metadata, 48, "");
    binary_string(&r.metadata, 56, "");
    return r;
}
void response_finish(BinaryReply *r) {
    if (r->metadata.size < RESPONSE_SIZE)
        return;
    put_u32(r->metadata.data + 16, (uint32_t)r->metadata.size);
    put_u64(r->metadata.data + 40, r->body.size);
}
BinaryReply response_error(uint32_t op) {
    char message[sizeof(failure)];
    snprintf(message, sizeof(message), "%s", failure);
    BinaryReply r = response_create(op, 0, 0);
    put_u32(r.metadata.data + 12, 1);
    binary_string(&r.metadata, 56, message);
    return r;
}
void binary_reply_free(BinaryReply *r) {
    bytes_free(&r->metadata);
    bytes_free(&r->body);
}
bool response_validate(BinaryReply r, uint32_t op) {
    Bytes b = r.metadata;
    if (b.size < RESPONSE_SIZE || get_u32(b.data) != RESPONSE_MAGIC || get_u32(b.data + 4) != 2 ||
        get_u32(b.data + 8) != op || get_u32(b.data + 16) != b.size || get_u64(b.data + 40) != r.body.size ||
        get_u32(b.data + 36)) {
        fail("invalid response header");
        return false;
    }
    uint32_t status = get_u32(b.data + 12), count = get_u32(b.data + 20), size = get_u32(b.data + 24),
             offset = get_u32(b.data + 28), variable = get_u32(b.data + 32);
    uint32_t expected = status               ? 0
                        : op == OP_LIST      ? FILE_ROW_SIZE
                        : op == OP_PROCESSES ? PROCESS_ROW_SIZE
                                             : 0;
    if (status > 1 || size != expected || offset != RESPONSE_SIZE || variable > b.size ||
        variable < RESPONSE_SIZE || (uint64_t)count * size != variable - RESPONSE_SIZE || (!size && count) ||
        ((status || op != OP_READ) && r.body.size)) {
        fail("invalid response records");
        return false;
    }
    char *path = binary_read_string(b, 48, variable, PATH_MAX - 1),
         *extra = binary_read_string(b, 56, variable, MAX_DATA);
    bool ok = path && extra;
    free(path);
    if (ok && status) {
        fail("%s", extra);
        ok = false;
    }
    free(extra);
    return ok;
}
