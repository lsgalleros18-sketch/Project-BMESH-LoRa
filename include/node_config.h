#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "bems_common.h"

typedef struct {
    bool configured;
    char node_id[FIELD_LEN];
    char node_name[FIELD_LEN];
    char node_role[FIELD_LEN];
    location_info_t location;
    char default_destination[FIELD_LEN];
    char default_priority[FIELD_LEN];
    char ap_password[FIELD_LEN];
    char web_pin[FIELD_LEN];
    char network_key[FIELD_LEN];
} node_config_t;

node_config_t *node_config_get(void);
const char *node_config_get_node_id(void);
const char *node_config_get_web_pin(void);
const char *node_config_get_network_key(void);
const char *node_config_get_ap_password(void);
const char *node_config_get_node_role(void);
const char *node_config_get_default_priority(void);
bool node_config_is_provisioned(void);
void node_config_set_identity(const char *node_id);
void node_config_load(void);
int node_config_save(const node_config_t *config);
void node_config_set_defaults(void);
void node_config_erase(void);
