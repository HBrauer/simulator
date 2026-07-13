#include "config.h"
#include "receiver.h"
#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

typedef struct {
    receiver_config_t *current_receiver;
    channel_profile_t *current_profile;
    channel_config_t *current_channel;
    bool in_receivers_seq;
    bool in_profiles_seq;
    bool in_channels_seq;
} parse_state_t;

static uint64_t parse_u64(const char *value)
{
    return strtoull(value, NULL, 10);
}

static int parse_int_value(const char *value)
{
    return (int)strtol(value, NULL, 10);
}

static double parse_double_value(const char *value)
{
    return strtod(value, NULL);
}

static bool parse_bool_value(const char *value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0;
}

/* Parse a CPU set written as a comma list and/or inclusive ranges, e.g. "2-7" or "0,2,4-6". */
static void parse_cpu_list(const char *value, int *out, size_t max, size_t *count)
{
    *count = 0;
    const char *p = value;
    while (*p != '\0' && *count < max) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *end = NULL;
        const long first = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        long last = first;
        if (*p == '-') {
            p++;
            last = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            p = end;
        }
        if (last < first) {
            const long tmp = first;
            last = first;
            (void)tmp;
        }
        for (long c = first; c <= last && *count < max; c++) {
            if (c >= 0) {
                out[(*count)++] = (int)c;
            }
        }
    }
}

static bool set_error(char *error, size_t error_size, const char *code)
{
    snprintf(error, error_size, "%s", code);
    return false;
}

/* The pre-channel config format is rejected with an error naming the replacement key so a
 * stale YAML fails loudly instead of being silently misread. */
static const char *legacy_receiver_key_error(const char *key)
{
    if (strcmp(key, "bandwidth_hz") == 0) {
        return "legacy_key_bandwidth_hz_use_frontend_bandwidth_hz";
    }
    if (strcmp(key, "sample_rate_hz") == 0) {
        return "legacy_key_sample_rate_hz_use_channel_profiles";
    }
    if (strcmp(key, "udp_80mhz_output_port") == 0) {
        return "legacy_key_udp_80mhz_output_port_use_channels";
    }
    if (strcmp(key, "stream_enabled") == 0) {
        return "legacy_key_stream_enabled_use_channels";
    }
    if (strcmp(key, "ddc_id") == 0) {
        return "legacy_key_ddc_use_channels";
    }
    return NULL;
}

static bool apply_scalar(simulator_config_t *config, parse_state_t *state, const char *key, const char *value, char *error, size_t error_size)
{
    if (state->current_profile != NULL) {
        if (strcmp(key, "bandwidth_hz") == 0) {
            state->current_profile->bandwidth_hz = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "sample_rate_hz") == 0) {
            state->current_profile->sample_rate_hz = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "name") == 0) {
            sim_strlcpy(state->current_profile->name, value, sizeof(state->current_profile->name));
        }
        return true;
    }

    if (state->current_channel != NULL) {
        if (strcmp(key, "channel_id") == 0) {
            state->current_channel->id = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "track_tuner") == 0) {
            state->current_channel->track_tuner = parse_bool_value(value);
        } else if (strcmp(key, "center_frequency_hz") == 0) {
            state->current_channel->center_frequency_hz = parse_u64(value);
        } else if (strcmp(key, "bandwidth_hz") == 0) {
            state->current_channel->bandwidth_hz = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "sample_rate_hz") == 0) {
            /* The rate always comes from the profile selected by bandwidth_hz. */
            return set_error(error, error_size, "channel_sample_rate_comes_from_profile");
        } else if (strcmp(key, "output_scale") == 0) {
            state->current_channel->output_scale = parse_double_value(value);
        } else if (strcmp(key, "rf_reference_power_dbm") == 0) {
            state->current_channel->rf_reference_power_dbm = parse_double_value(value);
        } else if (strcmp(key, "stream_enabled") == 0) {
            state->current_channel->stream_enabled = parse_bool_value(value);
        } else if (strcmp(key, "udp_output_port") == 0) {
            state->current_channel->udp_output.port = (uint16_t)parse_u64(value);
        } else if (strcmp(key, "ddc_id") == 0) {
            return set_error(error, error_size, "legacy_key_ddc_use_channels");
        }
        return true;
    }

    if (state->current_receiver != NULL) {
        receiver_config_t *r = state->current_receiver;
        const char *legacy = legacy_receiver_key_error(key);
        if (legacy != NULL) {
            return set_error(error, error_size, legacy);
        }
        if (strcmp(key, "receiver_id") == 0) {
            r->id = (uint32_t)parse_u64(value);
        } else if (strcmp(key, "rest_bind_host") == 0) {
            sim_strlcpy(r->rest_bind_host, value, sizeof(r->rest_bind_host));
        } else if (strcmp(key, "rest_port") == 0) {
            r->rest_port = (uint16_t)parse_u64(value);
        } else if (strcmp(key, "udp_output_host") == 0) {
            sim_strlcpy(r->udp_output_host, value, sizeof(r->udp_output_host));
        } else if (strcmp(key, "udp_multicast_interface") == 0) {
            sim_strlcpy(r->udp_multicast_interface, value, sizeof(r->udp_multicast_interface));
        } else if (strcmp(key, "frequency_start_hz") == 0) {
            r->frequency_start_hz = parse_u64(value);
        } else if (strcmp(key, "frequency_stop_hz") == 0) {
            r->frequency_stop_hz = parse_u64(value);
        } else if (strcmp(key, "frontend_bandwidth_hz") == 0) {
            r->frontend_bandwidth_hz = parse_u64(value);
        } else if (strcmp(key, "scan_rate_hz_per_s") == 0) {
            r->scan_rate_hz_per_s = parse_double_value(value);
        } else if (strcmp(key, "output_scale") == 0) {
            r->output_scale = parse_double_value(value);
        } else if (strcmp(key, "rf_reference_power_dbm") == 0) {
            r->rf_reference_power_dbm = parse_double_value(value);
        }
        return true;
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
    } else if (strcmp(key, "stream_cpu") == 0) {
        config->stream_cpu = parse_int_value(value);
    } else if (strcmp(key, "stream_cpus") == 0) {
        parse_cpu_list(value, config->stream_cpus, SIM_MAX_STREAM_CPUS, &config->stream_cpu_count);
    } else if (strcmp(key, "asset_cache_max_bytes") == 0) {
        config->asset_cache_max_bytes = (size_t)parse_u64(value);
    } else if (strcmp(key, "ddc_cache_max_bytes") == 0) {
        config->ddc_cache_max_bytes = (size_t)parse_u64(value);
    } else if (strcmp(key, "audio_prerender_oversample") == 0) {
        config->audio_prerender_oversample = parse_double_value(value);
    } else if (strcmp(key, "audio_prerender_max_rate_hz") == 0) {
        config->audio_prerender_max_rate_hz = (uint32_t)parse_u64(value);
    }
    return true;
}

static bool enter_sequence(simulator_config_t *config, parse_state_t *state, const char *key, char *error, size_t error_size)
{
    (void)config;
    if (strcmp(key, "receivers") == 0 && state->current_receiver == NULL) {
        state->in_receivers_seq = true;
    } else if (strcmp(key, "profiles") == 0 && state->current_receiver != NULL) {
        state->in_profiles_seq = true;
    } else if (strcmp(key, "channels") == 0 && state->current_receiver != NULL) {
        state->in_channels_seq = true;
    } else if (strcmp(key, "ddc") == 0) {
        return set_error(error, error_size, "legacy_key_ddc_use_channels");
    }
    return true;
}

static bool enter_mapping(simulator_config_t *config, parse_state_t *state, char *error, size_t error_size)
{
    if (state->in_profiles_seq && state->current_profile == NULL) {
        receiver_config_t *r = state->current_receiver;
        if (r->profile_count >= SIM_MAX_PROFILES) {
            return set_error(error, error_size, "too_many_profiles");
        }
        state->current_profile = &r->profiles[r->profile_count++];
    } else if (state->in_channels_seq && state->current_channel == NULL) {
        receiver_config_t *r = state->current_receiver;
        if (r->channel_count >= SIM_MAX_CHANNELS) {
            return set_error(error, error_size, "too_many_channels");
        }
        state->current_channel = &r->channels[r->channel_count++];
        state->current_channel->stream_enabled = true;
    } else if (state->in_receivers_seq && state->current_receiver == NULL) {
        if (config->receiver_count >= SIM_MAX_RECEIVERS) {
            return set_error(error, error_size, "too_many_receivers");
        }
        state->current_receiver = &config->receivers[config->receiver_count++];
    }
    return true;
}

static void leave_mapping(parse_state_t *state)
{
    if (state->current_profile != NULL) {
        state->current_profile = NULL;
    } else if (state->current_channel != NULL) {
        state->current_channel = NULL;
    } else if (!state->in_profiles_seq && !state->in_channels_seq && state->current_receiver != NULL) {
        state->current_receiver = NULL;
    }
}

static void leave_sequence(parse_state_t *state)
{
    if (state->in_profiles_seq) {
        state->in_profiles_seq = false;
    } else if (state->in_channels_seq) {
        state->in_channels_seq = false;
    } else if (state->in_receivers_seq) {
        state->in_receivers_seq = false;
    }
}

bool config_load_yaml(const char *path, simulator_config_t *config, char *error, size_t error_size)
{
    memset(config, 0, sizeof(*config));
    config->stream_cpu = -1;
    config->asset_cache_max_bytes = SIZE_MAX; /* unset sentinel; config_validate applies the default */
    config->ddc_cache_max_bytes = SIZE_MAX;   /* unset sentinel; config_validate applies the default */
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
            ok = enter_mapping(config, &state, error, error_size);
            expect_key = true;
        } else if (event.type == YAML_MAPPING_END_EVENT) {
            leave_mapping(&state);
            expect_key = true;
        } else if (event.type == YAML_SEQUENCE_START_EVENT) {
            ok = enter_sequence(config, &state, pending_key, error, error_size);
            expect_key = false;
        } else if (event.type == YAML_SEQUENCE_END_EVENT) {
            leave_sequence(&state);
            expect_key = true;
        } else if (event.type == YAML_SCALAR_EVENT) {
            const char *value = (const char *)event.data.scalar.value;
            if (expect_key) {
                sim_strlcpy(pending_key, value, sizeof(pending_key));
                expect_key = false;
            } else {
                ok = apply_scalar(config, &state, pending_key, value, error, error_size);
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
    /* Legacy single-CPU key maps to a one-element set when no stream_cpus list was given. */
    if (config->stream_cpu_count == 0 && config->stream_cpu >= 0) {
        config->stream_cpus[0] = config->stream_cpu;
        config->stream_cpu_count = 1;
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
    if (config->stream_cpu < -1) {
        snprintf(error, error_size, "invalid_stream_cpu");
        return false;
    }
    /* Audio pre-render tuning (§5). Defaults chosen so nothing must be set; a too-small
     * oversample would alias the modulation, so clamp up to the documented minimum. */
    if (config->audio_prerender_oversample <= 0.0) {
        config->audio_prerender_oversample = 2.0;
    }
    if (config->audio_prerender_oversample < 1.25) {
        config->audio_prerender_oversample = 1.25;
    }
    if (config->audio_prerender_max_rate_hz == 0U) {
        config->audio_prerender_max_rate_hz = 4000000U;
    }
    /* Default asset budget: 16 GiB. IQ files beyond it are mmap-backed rather than copied
     * (see asset_cache_load_limited); an explicit 0 keeps the legacy unlimited-RAM meaning. */
    if (config->asset_cache_max_bytes == SIZE_MAX) {
#if SIZE_MAX > 0xFFFFFFFFULL
        config->asset_cache_max_bytes = 16ULL << 30;
#else
        config->asset_cache_max_bytes = SIZE_MAX / 2;
#endif
    }
    /* Default budget for precomputed DDC intermediates (see ddc_cache.h). One entry holds a
     * full source loop at its intermediate rate as ci16 (a 60 s loop at 1.536 MS/s is
     * ~369 MB); an explicit 0 disables caching, so DDC channels run the direct full-rate
     * cascade every block. */
    if (config->ddc_cache_max_bytes == SIZE_MAX) {
#if SIZE_MAX > 0xFFFFFFFFULL
        config->ddc_cache_max_bytes = 2ULL << 30;
#else
        config->ddc_cache_max_bytes = SIZE_MAX / 4;
#endif
    }
    for (size_t i = 0; i < config->receiver_count; i++) {
        receiver_config_t *receiver = &config->receivers[i];
        if (receiver->output_scale == 0.0) {
            receiver->output_scale = 1.0;
        }
        if (receiver->rf_reference_power_dbm == 0.0) {
            receiver->rf_reference_power_dbm = -55.0;
        }
        if (receiver->frontend_bandwidth_hz == 0ULL) {
            receiver->frontend_bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ;
        }
        if (receiver->profile_count == 0) {
            receiver_default_profiles(receiver);
        }
        for (size_t c = 0; c < receiver->channel_count; c++) {
            channel_config_t *channel = &receiver->channels[c];
            if (channel->bandwidth_hz == 0U) {
                snprintf(error, error_size, "channel_bandwidth_required");
                return false;
            }
            /* The sample rate is always the profile partner of the selected bandwidth. */
            const channel_profile_t *profile = receiver_find_profile(receiver, channel->bandwidth_hz);
            if (profile == NULL) {
                snprintf(error, error_size, "unsupported_channel_bandwidth");
                return false;
            }
            channel->sample_rate_hz = profile->sample_rate_hz;
            if (channel->output_scale == 0.0) {
                channel->output_scale = receiver->output_scale;
            }
            if (channel->rf_reference_power_dbm == 0.0) {
                channel->rf_reference_power_dbm = receiver->rf_reference_power_dbm;
            }
        }
        if (!receiver_validate(receiver, error, error_size)) {
            return false;
        }
        for (size_t p = 0; p < receiver->channel_count; p++) {
            for (size_t q = p + 1; q < receiver->channel_count; q++) {
                if (receiver->channels[p].udp_output.port == receiver->channels[q].udp_output.port) {
                    snprintf(error, error_size, "duplicate_udp_port");
                    return false;
                }
            }
        }
        for (size_t j = i + 1; j < config->receiver_count; j++) {
            const receiver_config_t *other = &config->receivers[j];
            if (receiver->id == other->id) {
                snprintf(error, error_size, "duplicate_receiver");
                return false;
            }
            if (receiver->rest_port == other->rest_port) {
                snprintf(error, error_size, "duplicate_rest_port");
                return false;
            }
            for (size_t p = 0; p < receiver->channel_count; p++) {
                for (size_t q = 0; q < other->channel_count; q++) {
                    if (receiver->channels[p].udp_output.port == other->channels[q].udp_output.port) {
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
