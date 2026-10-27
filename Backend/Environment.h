#ifndef APP_ENVIRONMENT_H
#define APP_ENVIRONMENT_H
#include <stddef.h>
char *environment_request(const void *body, size_t length, size_t *result_length);
#endif
