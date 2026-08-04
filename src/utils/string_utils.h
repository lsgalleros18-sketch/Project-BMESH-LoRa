#pragma once

#include <stddef.h>
#include <stdbool.h>

void copy_field(char *destination, size_t destination_size, const char *source);
void copy_field_no_delims(char *destination, size_t destination_size, const char *source);
void compute_thread_key(char *out, size_t out_size, const char *source, const char *destination);
bool form_value(const char *body, const char *key, char *output, size_t output_size);
