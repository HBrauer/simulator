#ifndef CONFIG_H
#define CONFIG_H

#include "sim_types.h"

#include <stdbool.h>
#include <stdio.h>

bool config_load_yaml(const char *path, simulator_config_t *config, char *error, size_t error_size);
/* Same as config_load_yaml, reading from an open stream (left open). */
bool config_load_yaml_stream(FILE *file, simulator_config_t *config, char *error, size_t error_size);
bool config_validate(simulator_config_t *config, char *error, size_t error_size);

#endif
