#include "../Backend/Environment.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    size_t capacity = 16u * 1024u * 1024u, length = 0;
    unsigned char *input = malloc(capacity);
    if (!input) return 1;
    while (length < capacity) {
        size_t count = fread(input + length, 1, capacity - length, stdin);
        length += count;
        if (!count) break;
    }
    if (ferror(stdin) || (length == capacity && fgetc(stdin) != EOF)) { free(input); return 1; }
    size_t result_length = 0;
    char *result = environment_request(input, length, &result_length);
    free(input);
    if (!result) return 1;
    int code = fwrite(result, 1, result_length, stdout) == result_length ? 0 : 1;
    free(result);
    return code;
}
