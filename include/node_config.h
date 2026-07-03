#pragma once

#include <stdbool.h>
#include <stddef.h>

#define FIELD_LEN 32
#define SITIO_LEN 24
#define BARANGAY_LEN 24
#define MUNICIPALITY_LEN 24

typedef struct {
    char sitio[SITIO_LEN];
    char barangay[BARANGAY_LEN];
    char municipality[MUNICIPALITY_LEN];
} location_info_t;

typedef struct {
    bool configured;
    char node_id[FIELD_LEN];
    char node_name[FIELD_LEN];
    location_info_t location;
    char default_destination[FIELD_LEN];
    char web_pin[FIELD_LEN];
    char network_key[FIELD_LEN];
} node_config_t;

// Global configuration
extern node_config_t node_config;
extern char node_id[FIELD_LEN];

// Loads node configuration from NVS
void load_node_config(void);

// Saves node configuration to NVS
int save_node_config(const node_config_t *config);

// Sets default configuration values
void config_set_defaults(void);

// Erases stored configuration
void erase_node_config(void);
