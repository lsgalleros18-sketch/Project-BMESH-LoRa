#include "utils/json_utils.h"

#include <stdint.h>

static void append(char *destination, size_t capacity, size_t *used, const char *text, size_t length)
{
    if (*used + length >= capacity) {
        return;
    }
    for (size_t i = 0; i < length; i++) destination[(*used)++] = text[i];
}

void json_escape_string(char *destination, size_t destination_size, const char *source)
{
    size_t used = 0;
    static const char hex[] = "0123456789ABCDEF";

    if (destination_size == 0) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    while (*source != '\0') {
        unsigned char c = (unsigned char)*source++;
        char escape[6];
        switch (c) {
        case '"': append(destination, destination_size, &used, "\\\"", 2); break;
        case '\\': append(destination, destination_size, &used, "\\\\", 2); break;
        case '\b': append(destination, destination_size, &used, "\\b", 2); break;
        case '\f': append(destination, destination_size, &used, "\\f", 2); break;
        case '\n': append(destination, destination_size, &used, "\\n", 2); break;
        case '\r': append(destination, destination_size, &used, "\\r", 2); break;
        case '\t': append(destination, destination_size, &used, "\\t", 2); break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                escape[0] = '\\'; escape[1] = 'u'; escape[2] = '0'; escape[3] = '0';
                escape[4] = hex[c >> 4]; escape[5] = hex[c & 0x0f];
                append(destination, destination_size, &used, escape, sizeof(escape));
            } else {
                append(destination, destination_size, &used, (const char *)&c, 1);
            }
            break;
        }
        if (used + 1 >= destination_size) break;
    }
    destination[used] = '\0';
}
