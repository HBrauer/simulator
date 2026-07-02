#ifndef SCENARIO_H
#define SCENARIO_H

#include "sim_types.h"

#include <stdbool.h>

bool scenario_load_json(const char *path, scenario_t *scenario, char *error, size_t error_size);
const scenario_source_t *scenario_find_source(const scenario_t *scenario, const char *source_id);
bool scenario_validate(scenario_t *scenario, const char *base_dir, char *error, size_t error_size);

#endif
