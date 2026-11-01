#ifndef APP_ENVIRONMENT_H
#define APP_ENVIRONMENT_H
#include <stddef.h>
void environment_registry_socket(char *path, size_t size);
char *environment_request(const void *body, size_t length, size_t *result_length);
// Finds this app's container helpers in the helpers folder beside web_root.
void environment_set_web_root(const char *web_root);
#endif
