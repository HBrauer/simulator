#ifndef SCENARIO_H
#define SCENARIO_H

#include "sim_types.h"

#include <stdbool.h>
#include <stdio.h>

bool scenario_load(const char *path, scenario_t *scenario, char *error, size_t error_size);
bool scenario_load_stream(FILE *file, scenario_t *scenario, char *error, size_t error_size); /* leaves file open */
const scenario_source_t *scenario_find_source(const scenario_t *scenario, const char *source_id);
bool scenario_validate(scenario_t *scenario, const char *base_dir, char *error, size_t error_size);

#endif
