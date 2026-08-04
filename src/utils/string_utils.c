#include "utils/string_utils.h"

#include <ctype.h>
#include <string.h>
#include <sys/param.h>

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *read_ptr = value;
    char *write_ptr = value;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+') {
            *write_ptr++ = ' ';
            read_ptr++;
        } else if (*read_ptr == '%' && isxdigit((unsigned char)read_ptr[1]) && isxdigit((unsigned char)read_ptr[2])) {
            *write_ptr++ = (char)((hex_value(read_ptr[1]) << 4) | hex_value(read_ptr[2]));
            read_ptr += 3;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }

    *write_ptr = '\0';
}

void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

void copy_field_no_delims(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char c = (unsigned char)*source++;
        if (c >= 32 && c <= 126 && c != '|' && c != '~') {
            destination[write_index++] = (char)c;
        }
    }

    destination[write_index] = '\0';
}

void compute_thread_key(char *out, size_t out_size, const char *source, const char *destination)
{
    if (out_size == 0) {
        return;
    }

    if (strcmp(destination, "ALL") == 0) {
        copy_field(out, out_size, "ANNOUNCEMENTS");
        return;
    }

    copy_field(out, out_size, source);
}

bool form_value(const char *body, const char *key, char *output, size_t output_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            size_t value_len = value_end == NULL ? strlen(value_start) : (size_t)(value_end - value_start);
            size_t copy_len = MIN(value_len, output_size - 1);

            memcpy(output, value_start, copy_len);
            output[copy_len] = '\0';
            url_decode(output);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }

    return false;
}
