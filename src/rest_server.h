#ifndef REST_SERVER_H
#define REST_SERVER_H

#include "sim_types.h"
#include "timebase.h"

#include <stdbool.h>

typedef struct rest_server rest_server_t;

typedef struct {
    receiver_config_t *receiver;
    const scenario_t *scenario;
    const timebase_t *timebase;
    const char *version;
} rest_context_t;

bool rest_server_start(rest_server_t **server, const rest_context_t *context);
void rest_server_stop(rest_server_t *server);

#endif
