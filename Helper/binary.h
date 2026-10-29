#ifndef ENVIRONMENT_BINARY_H
#define ENVIRONMENT_BINARY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
enum { OP_LIST = 1, OP_READ, OP_SAVE, OP_MOVE, OP_PROCESSES, OP_SIGNAL, OP_PLACES = 10, OP_ADD_PLACE = 11 };
enum { REQUEST_SIZE = 80, RESPONSE_SIZE = 64, FILE_ROW_SIZE = 48, PROCESS_ROW_SIZE = 96 };
#define REQUEST_MAGIC 0x52484c46u
#define RESPONSE_MAGIC 0x53484c46u
typedef struct {
    uint32_t operation, pid, signal;
    uint64_t limit, data_length;
    char *path, *destination, *revision, *start;
} Request;
typedef struct {
    Bytes metadata, body;
} BinaryReply;
uint32_t get_u32(const unsigned char *p);
uint64_t get_u64(const unsigned char *p);
void put_u32(unsigned char *p, uint32_t n);
void put_u64(unsigned char *p, uint64_t n);
bool binary_string(Bytes *bytes, size_t field, const char *value);
char *binary_read_string(Bytes bytes, size_t field, size_t variable, size_t maximum);
Bytes request_encode(const Request *request);
bool request_decode(Bytes bytes, Request *request);
void request_free(Request *request);
BinaryReply response_create(uint32_t op, uint32_t count, uint32_t row_size);
void response_finish(BinaryReply *reply);
BinaryReply response_error(uint32_t op);
bool response_validate(BinaryReply reply, uint32_t op);
void binary_reply_free(BinaryReply *reply);
BinaryReply local_binary_operation(const Request *request, Bytes body);
#endif
