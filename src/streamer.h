#ifndef STREAMER_H
#define STREAMER_H

#include "sim_types.h"
#include "timebase.h"

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

typedef struct streamer_manager streamer_manager_t;

typedef struct {
    simulator_config_t *config;
    const scenario_t *scenario;
    const timebase_t *timebase;
    pthread_mutex_t *receiver_lock;
    size_t block_samples;
} streamer_config_t;

bool streamer_manager_start(streamer_manager_t **manager, const streamer_config_t *config);
void streamer_manager_stop(streamer_manager_t *manager);

#endif
