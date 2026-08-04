#include "utils/json_utils.h"

void json_escape_string(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        char character = *source++;

        if ((character == '"' || character == '\\') && write_index < destination_size - 2) {
            destination[write_index++] = '\\';
            destination[write_index++] = character;
        } else if (character != '"' && character != '\\') {
            destination[write_index++] = character;
        } else {
            break;
        }
    }

    destination[write_index] = '\0';
}
