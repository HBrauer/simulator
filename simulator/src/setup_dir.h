#ifndef SETUP_DIR_H
#define SETUP_DIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A setup folder holds one simulator setup: receiver.yaml, scenario.yaml and assets/. Relative
 * asset paths in the scenario resolve against the folder, so a setup can live anywhere. */
#define SETUP_DIR_ENV "SDR_SIMULATOR_CONFIG_DIR"
#define SETUP_DIR_CONFIG_FILE "receiver.yaml"
#define SETUP_DIR_SCENARIO_FILE "scenario.yaml"
#define SETUP_DIR_ASSETS_DIR "assets"

typedef struct {
    const char *name;
    const char *contents;
} setup_template_file_t;

/* Starter files embedded at build time from simulator/setup_template/. */
extern const setup_template_file_t setup_template_files[];
extern const size_t setup_template_file_count;

/* Opens the embedded starter file `name` as a read-only stream, or NULL. */
FILE *setup_template_open(const char *name);

/* Create dir (and parents) with the starter files and an empty assets/ folder. Refuses to
 * overwrite a starter file that already exists, so re-running on a live setup is harmless. */
bool setup_dir_init(const char *dir, char *error, size_t error_size);

#endif
