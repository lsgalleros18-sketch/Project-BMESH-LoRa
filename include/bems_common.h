#pragma once

#define FIELD_LEN 32
#define SITIO_LEN 24
#define BARANGAY_LEN 24
#define MUNICIPALITY_LEN 24
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
    char sitio[SITIO_LEN];
    char barangay[BARANGAY_LEN];
    char municipality[MUNICIPALITY_LEN];
} location_info_t;
