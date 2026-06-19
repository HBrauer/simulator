#ifndef CONFIG_H
#define CONFIG_H

#include "sim_types.h"

#include <stdbool.h>

bool config_load_yaml(const char *path, simulator_config_t *config, char *error, size_t error_size);
bool config_validate(const simulator_config_t *config, char *error, size_t error_size);

#endif
