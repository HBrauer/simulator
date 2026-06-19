#include "config.h"
#include "renderer.h"
#include "rest_server.h"
#include "scenario.h"
#include "streamer.h"
#include "timebase.h"
#include "udp_output.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void on_signal(int signum)
{
    (void)signum;
    keep_running = 0;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *config_path = arg_value(argc, argv, "--config");
    const char *scenario_path = arg_value(argc, argv, "--scenario");
    const char *scenario_time_arg = arg_value(argc, argv, "--scenario-time-ns");
    const char *once_arg = arg_value(argc, argv, "--render-once-samples");
    const char *block_samples_arg = arg_value(argc, argv, "--stream-block-samples");
    if (config_path == NULL) {
        config_path = "configs/instance_001.yaml";
    }

    char error[256];
    simulator_config_t config;
    if (!config_load_yaml(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 2;
    }
    if (scenario_path == NULL) {
        scenario_path = config.scenario_file;
    }
    scenario_t scenario;
    if (!scenario_load_json(scenario_path, &scenario, error, sizeof(error)) ||
        !scenario_validate(&scenario, ".", error, sizeof(error))) {
        fprintf(stderr, "scenario error: %s\n", error);
        return 3;
    }

    timebase_t timebase;
    timebase_init(&timebase);
    if (scenario_time_arg != NULL) {
        timebase_set_override(&timebase, strtoull(scenario_time_arg, NULL, 10));
    }

    if (once_arg != NULL) {
        const size_t samples = (size_t)strtoull(once_arg, NULL, 10);
        iq_ci16_t *buffer = calloc(samples, sizeof(*buffer));
        if (buffer == NULL) {
            return 4;
        }
        render_stats_t stats;
        renderer_render_80mhz_block(&scenario, &config.receivers[0], timebase_now_ns(&timebase), buffer, samples, &stats);
        fwrite(buffer, sizeof(*buffer), samples, stdout);
        free(buffer);
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    size_t block_samples = 1024;
    if (block_samples_arg != NULL) {
        block_samples = (size_t)strtoull(block_samples_arg, NULL, 10);
        if (block_samples == 0 || block_samples > 4096) {
            fprintf(stderr, "invalid --stream-block-samples\n");
            return 6;
        }
    }

    rest_server_t *servers[SIM_MAX_RECEIVERS] = {0};
    for (size_t i = 0; i < config.receiver_count; i++) {
        rest_context_t context = {
            .receiver = &config.receivers[i],
            .scenario = &scenario,
            .timebase = &timebase,
            .version = "0.1.0",
        };
        if (!rest_server_start(&servers[i], &context)) {
            fprintf(stderr, "failed to start REST server for receiver %u\n", config.receivers[i].id);
            return 5;
        }
        printf("receiver %u REST http://%s:%u/api/v1\n", config.receivers[i].id, config.receivers[i].rest_bind_host, config.receivers[i].rest_port);
    }

    streamer_manager_t *streamer = NULL;
    streamer_config_t streamer_config = {
        .config = &config,
        .scenario = &scenario,
        .timebase = &timebase,
        .block_samples = block_samples,
    };
    if (!streamer_manager_start(&streamer, &streamer_config)) {
        fprintf(stderr, "failed to start UDP streamers\n");
        for (size_t i = 0; i < config.receiver_count; i++) {
            rest_server_stop(servers[i]);
        }
        return 7;
    }

    while (keep_running) {
        sleep(1);
    }

    streamer_manager_stop(streamer);
    for (size_t i = 0; i < config.receiver_count; i++) {
        rest_server_stop(servers[i]);
    }
    return 0;
}
