#include "config.h"
#include "receiver.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

typedef struct {
    receiver_config_t *current_receiver;
    ddc_config_t *current_ddc;
} parse_state_t;

static uint64_t parse_u64(const char *value)
{
    return strtoull(value, NULL, 10);
}

static double parse_double_value(const char *value)
{
    return strtod(value, NULL);
}

static void apply_scalar(simulator_config_t *config, parse_state_t *state, const char *key, const char *value)
{
    if (state->current_ddc != NULL) {
        if (strcmp(key, "ddc_id") == 0) {
            state->current_ddc->id = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "center_frequency_hz") == 0) {
            state->current_ddc->center_frequency_hz = parse_u64(value);
        } else if (strcmp(key, "output_scale") == 0) {
            state->current_ddc->output_scale = parse_double_value(value);
        } else if (strcmp(key, "udp_output_port") == 0) {
            state->current_ddc->udp_output.port = (uint16_t)parse_u64(value);
        }
        state->current_ddc->bandwidth_hz = SIM_DDC_BANDWIDTH_HZ;
        state->current_ddc->sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ;
        return;
    }

    if (state->current_receiver != NULL) {
        receiver_config_t *r = state->current_receiver;
        if (strcmp(key, "receiver_id") == 0) {
            r->id = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "rest_bind_host") == 0) {
            sim_strlcpy(r->rest_bind_host, value, sizeof(r->rest_bind_host));
        } else if (strcmp(key, "rest_port") == 0) {
            r->rest_port = (uint16_t)parse_u64(value);
        } else if (strcmp(key, "udp_output_host") == 0) {
            sim_strlcpy(r->udp_output_host, value, sizeof(r->udp_output_host));
        } else if (strcmp(key, "frequency_start_hz") == 0) {
            r->frequency_start_hz = parse_u64(value);
        } else if (strcmp(key, "frequency_stop_hz") == 0) {
            r->frequency_stop_hz = parse_u64(value);
        } else if (strcmp(key, "scan_rate_hz_per_s") == 0) {
            r->scan_rate_hz_per_s = parse_double_value(value);
        } else if (strcmp(key, "output_scale") == 0) {
            r->output_scale = parse_double_value(value);
        } else if (strcmp(key, "udp_80mhz_output_port") == 0) {
            r->udp_80mhz_output.port = (uint16_t)parse_u64(value);
        }
        r->bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ;
        r->sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ;
        return;
    }

    if (strcmp(key, "schema_version") == 0) {
        config->schema_version = (int)parse_u64(value);
    } else if (strcmp(key, "instance_id") == 0) {
        sim_strlcpy(config->instance_id, value, sizeof(config->instance_id));
    } else if (strcmp(key, "scenario_file") == 0) {
        sim_strlcpy(config->scenario_file, value, sizeof(config->scenario_file));
    } else if (strcmp(key, "log_path") == 0) {
        sim_strlcpy(config->log_path, value, sizeof(config->log_path));
    } else if (strcmp(key, "stream_block_samples") == 0) {
        config->stream_block_samples = (size_t)parse_u64(value);
    }
}

bool config_load_yaml(const char *path, simulator_config_t *config, char *error, size_t error_size)
{
    memset(config, 0, sizeof(*config));
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "config_not_found");
        return false;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(file);
        snprintf(error, error_size, "yaml_parser_init_failed");
        return false;
    }
    yaml_parser_set_input_file(&parser, file);

    parse_state_t state;
    memset(&state, 0, sizeof(state));
    char pending_key[64] = "";
    bool expect_key = true;
    bool ok = true;

    while (ok) {
        yaml_event_t event;
        if (!yaml_parser_parse(&parser, &event)) {
            snprintf(error, error_size, "config_yaml_invalid");
            ok = false;
            break;
        }
        if (event.type == YAML_STREAM_END_EVENT) {
            yaml_event_delete(&event);
            break;
        }
        if (event.type == YAML_MAPPING_START_EVENT) {
            expect_key = true;
        } else if (event.type == YAML_MAPPING_END_EVENT) {
            if (state.current_ddc != NULL) {
                state.current_ddc = NULL;
            } else {
                state.current_receiver = NULL;
            }
            expect_key = true;
        } else if (event.type == YAML_SEQUENCE_START_EVENT) {
            expect_key = false;
        } else if (event.type == YAML_SEQUENCE_END_EVENT) {
            state.current_ddc = NULL;
            expect_key = true;
        } else if (event.type == YAML_SCALAR_EVENT) {
            const char *value = (const char *)event.data.scalar.value;
            if (expect_key) {
                sim_strlcpy(pending_key, value, sizeof(pending_key));
                expect_key = false;
            } else {
                if (strcmp(pending_key, "receiver_id") == 0) {
                    if (config->receiver_count >= SIM_MAX_RECEIVERS) {
                        snprintf(error, error_size, "too_many_receivers");
                        ok = false;
                    } else {
                        state.current_receiver = &config->receivers[config->receiver_count++];
                    }
                } else if (strcmp(pending_key, "ddc_id") == 0 && state.current_receiver != NULL) {
                    const uint32_t id = (uint32_t)parse_u64(value);
                    if (id < SIM_DDC_COUNT) {
                        state.current_ddc = &state.current_receiver->ddc[id];
                    }
                }
                if (ok) {
                    apply_scalar(config, &state, pending_key, value);
                }
                expect_key = true;
            }
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(file);
    if (!ok) {
        return false;
    }
    return config_validate(config, error, error_size);
}

bool config_validate(simulator_config_t *config, char *error, size_t error_size)
{
    if (config->schema_version != 1 || config->receiver_count == 0) {
        snprintf(error, error_size, "config_invalid");
        return false;
    }
    if (config->stream_block_samples == 0) {
        config->stream_block_samples = SIM_DEFAULT_STREAM_BLOCK_SAMPLES;
    }
    if (config->stream_block_samples > SIM_MAX_STREAM_BLOCK_SAMPLES) {
        snprintf(error, error_size, "invalid_stream_block_samples");
        return false;
    }
    for (size_t i = 0; i < config->receiver_count; i++) {
        if (config->receivers[i].output_scale == 0.0) {
            config->receivers[i].output_scale = 1.0;
        }
        for (size_t d = 0; d < SIM_DDC_COUNT; d++) {
            if (config->receivers[i].ddc[d].output_scale == 0.0) {
                config->receivers[i].ddc[d].output_scale = config->receivers[i].output_scale;
            }
        }
        if (!receiver_validate(&config->receivers[i], error, error_size)) {
            return false;
        }
        const receiver_config_t *receiver = &config->receivers[i];
        uint16_t ports[1 + SIM_DDC_COUNT];
        ports[0] = receiver->udp_80mhz_output.port;
        for (size_t d = 0; d < SIM_DDC_COUNT; d++) {
            ports[1 + d] = receiver->ddc[d].udp_output.port;
        }
        for (size_t p = 0; p < 1 + SIM_DDC_COUNT; p++) {
            for (size_t q = p + 1; q < 1 + SIM_DDC_COUNT; q++) {
                if (ports[p] == ports[q]) {
                    snprintf(error, error_size, "duplicate_udp_port");
                    return false;
                }
            }
        }
        for (size_t j = i + 1; j < config->receiver_count; j++) {
            if (config->receivers[i].id == config->receivers[j].id) {
                snprintf(error, error_size, "duplicate_receiver");
                return false;
            }
            if (config->receivers[i].rest_port == config->receivers[j].rest_port) {
                snprintf(error, error_size, "duplicate_rest_port");
                return false;
            }
            const receiver_config_t *other = &config->receivers[j];
            uint16_t other_ports[1 + SIM_DDC_COUNT];
            other_ports[0] = other->udp_80mhz_output.port;
            for (size_t d = 0; d < SIM_DDC_COUNT; d++) {
                other_ports[1 + d] = other->ddc[d].udp_output.port;
            }
            for (size_t p = 0; p < 1 + SIM_DDC_COUNT; p++) {
                for (size_t q = 0; q < 1 + SIM_DDC_COUNT; q++) {
                    if (ports[p] == other_ports[q]) {
                        snprintf(error, error_size, "duplicate_udp_port");
                        return false;
                    }
                }
            }
        }
    }
    snprintf(error, error_size, "ok");
    return true;
}
