#pragma once

#include <stddef.h>

void json_escape_string(char *destination, size_t destination_size, const char *source);
