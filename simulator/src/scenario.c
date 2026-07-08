#include "scenario.h"
#include "iq_file_reader.h"
#include "util.h"
#include "wav_reader.h"

#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool get_json_string(json_t *object, const char *key, char *dst, size_t dst_size)
{
    json_t *value = json_object_get(object, key);
    if (!json_is_string(value)) {
        return false;
    }
    sim_strlcpy(dst, json_string_value(value), dst_size);
    return true;
}

static bool get_json_u64(json_t *object, const char *key, uint64_t *out)
{
    json_t *value = json_object_get(object, key);
    if (!json_is_integer(value)) {
        return false;
    }
    json_int_t v = json_integer_value(value);
    if (v < 0) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool get_json_u32(json_t *object, const char *key, uint32_t *out)
{
    uint64_t v = 0;
    if (!get_json_u64(object, key, &v) || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool get_json_double(json_t *object, const char *key, double *out)
{
    json_t *value = json_object_get(object, key);
    if (json_is_real(value) || json_is_integer(value)) {
        *out = json_number_value(value);
        return true;
    }
    return false;
}

static bool parse_source_kind(const char *source_type, scenario_source_kind_t *out)
{
    if (strcmp(source_type, "iq_file") == 0) {
        *out = SCENARIO_SOURCE_IQ_FILE;
        return true;
    }
    if (strcmp(source_type, "audio_file") == 0) {
        *out = SCENARIO_SOURCE_AUDIO_FILE;
        return true;
    }
    return false;
}

static bool parse_modulation(const char *name, scenario_modulation_t *out)
{
    if (strcmp(name, "iq") == 0) {
        *out = SCENARIO_MODULATION_IQ;
        return true;
    }
    if (strcmp(name, "wbfm") == 0) {
        *out = SCENARIO_MODULATION_WBFM;
        return true;
    }
    if (strcmp(name, "am") == 0) {
        *out = SCENARIO_MODULATION_AM;
        return true;
    }
    if (strcmp(name, "usb") == 0) {
        *out = SCENARIO_MODULATION_USB;
        return true;
    }
    if (strcmp(name, "lsb") == 0) {
        *out = SCENARIO_MODULATION_LSB;
        return true;
    }
    return false;
}

static bool parse_replay_mode(const char *name, scenario_replay_mode_t *out)
{
    if (strcmp(name, "fixed") == 0) {
        *out = SCENARIO_REPLAY_FIXED;
        return true;
    }
    if (strcmp(name, "range") == 0) {
        *out = SCENARIO_REPLAY_RANGE;
        return true;
    }
    if (strcmp(name, "shift") == 0) {
        *out = SCENARIO_REPLAY_SHIFT;
        return true;
    }
    return false;
}

bool scenario_load_json(const char *path, scenario_t *scenario, char *error, size_t error_size)
{
    memset(scenario, 0, sizeof(*scenario));
    json_error_t json_error;
    json_t *root = json_load_file(path, 0, &json_error);
    if (root == NULL) {
        snprintf(error, error_size, "scenario_invalid:%s", json_error.text);
        return false;
    }
    scenario->schema_version = (int)json_integer_value(json_object_get(root, "schema_version"));
    if (!get_json_string(root, "scenario_id", scenario->scenario_id, sizeof(scenario->scenario_id))) {
        json_decref(root);
        snprintf(error, error_size, "scenario_missing_id");
        return false;
    }
    json_t *desc = json_object_get(root, "description");
    if (json_is_string(desc)) {
        sim_strlcpy(scenario->description, json_string_value(desc), sizeof(scenario->description));
    }
    json_t *noise = json_object_get(root, "noise_floor");
    if (json_is_object(noise)) {
        json_t *enabled = json_object_get(noise, "enabled");
        scenario->noise_floor.enabled = enabled == NULL ? true : json_is_true(enabled);
        if (scenario->noise_floor.enabled) {
            const bool has_density = get_json_double(noise, "power_dbm_per_hz", &scenario->noise_floor.power_dbm_per_hz);
            const bool has_total = get_json_double(noise, "power_dbm", &scenario->noise_floor.power_dbm);
            if (has_density && has_total) {
                json_decref(root);
                snprintf(error, error_size, "noise_floor_conflicting_power");
                return false;
            }
            if (!has_density && !has_total) {
                json_decref(root);
                snprintf(error, error_size, "noise_floor_invalid");
                return false;
            }
            scenario->noise_floor.use_density = has_density;
            if (!get_json_u64(noise, "seed", &scenario->noise_floor.seed)) {
                scenario->noise_floor.seed = 1ULL;
            }
        }
    }

    json_t *sources = json_object_get(root, "sources");
    json_t *signals = json_object_get(root, "signals");
    if (!json_is_array(sources) || !json_is_array(signals)) {
        json_decref(root);
        snprintf(error, error_size, "scenario_missing_arrays");
        return false;
    }
    scenario->source_count = json_array_size(sources);
    scenario->signal_count = json_array_size(signals);
    if (scenario->source_count > SIM_MAX_SOURCES || scenario->signal_count > SIM_MAX_SIGNALS) {
        json_decref(root);
        snprintf(error, error_size, "scenario_too_large");
        return false;
    }

    for (size_t i = 0; i < scenario->source_count; i++) {
        json_t *src = json_array_get(sources, i);
        scenario_source_t *out = &scenario->sources[i];
        if (!get_json_string(src, "id", out->id, sizeof(out->id)) ||
            !get_json_string(src, "source_type", out->source_type, sizeof(out->source_type)) ||
            !get_json_string(src, "file", out->file, sizeof(out->file)) ||
            !get_json_string(src, "format", out->format, sizeof(out->format)) ||
            !get_json_u32(src, "sample_rate_hz", &out->sample_rate_hz) ||
            !get_json_u32(src, "bandwidth_hz", &out->bandwidth_hz) ||
            !get_json_double(src, "nominal_level_dbfs", &out->nominal_level_dbfs)) {
            json_decref(root);
            snprintf(error, error_size, "source_invalid");
            return false;
        }
        if (!parse_source_kind(out->source_type, &out->source_kind)) {
            json_decref(root);
            snprintf(error, error_size, "source_unsupported");
            return false;
        }
        if (out->source_kind == SCENARIO_SOURCE_IQ_FILE) {
            if (!get_json_string(src, "byte_order", out->byte_order, sizeof(out->byte_order)) ||
                !get_json_string(src, "iq_layout", out->iq_layout, sizeof(out->iq_layout))) {
                json_decref(root);
                snprintf(error, error_size, "source_invalid");
                return false;
            }
        }
        if (!get_json_u64(src, "sample_count", &out->sample_count)) {
            out->sample_count = 0;
        }
        out->center_frequency_hz = json_integer_value(json_object_get(src, "center_frequency_hz"));
    }

    for (size_t i = 0; i < scenario->signal_count; i++) {
        json_t *sig = json_array_get(signals, i);
        scenario_signal_t *out = &scenario->signals[i];
        char replay_mode_name[16];
        if (!get_json_string(sig, "replay_mode", replay_mode_name, sizeof(replay_mode_name))) {
            sim_strlcpy(replay_mode_name, "fixed", sizeof(replay_mode_name));
        }
        if (!parse_replay_mode(replay_mode_name, &out->replay_mode)) {
            json_decref(root);
            snprintf(error, error_size, "signal_replay_mode_invalid");
            return false;
        }
        json_t *range = json_object_get(sig, "frequency_range");
        if (json_is_object(range)) {
            if (!get_json_u64(range, "start_hz", &out->replay_range_start_hz) ||
                !get_json_u64(range, "stop_hz", &out->replay_range_stop_hz)) {
                json_decref(root);
                snprintf(error, error_size, "signal_frequency_range_invalid");
                return false;
            }
        } else if (range != NULL) {
            json_decref(root);
            snprintf(error, error_size, "signal_frequency_range_invalid");
            return false;
        }
        json_t *loop = json_object_get(sig, "loop");
        if (json_is_boolean(loop)) {
            out->loop = json_is_true(loop);
        } else {
            out->loop = out->replay_mode != SCENARIO_REPLAY_FIXED;
        }
        json_t *passthrough = json_object_get(sig, "passthrough");
        out->passthrough = json_is_boolean(passthrough) && json_is_true(passthrough);
        if (!get_json_string(sig, "signal_id", out->signal_id, sizeof(out->signal_id)) ||
            !get_json_string(sig, "source_reference", out->source_reference, sizeof(out->source_reference)) ||
            !get_json_u32(sig, "bandwidth_hz", &out->bandwidth_hz) ||
            !get_json_double(sig, "power_dbm", &out->power_dbm)) {
            json_decref(root);
            snprintf(error, error_size, "signal_invalid");
            return false;
        }
        /* center_frequency_hz is ignored in range mode (content follows the tune) and may be
         * omitted there; every other mode needs the absolute placement. */
        if (!get_json_u64(sig, "center_frequency_hz", &out->center_frequency_hz) &&
            out->replay_mode != SCENARIO_REPLAY_RANGE) {
            json_decref(root);
            snprintf(error, error_size, "signal_invalid");
            return false;
        }
        if (!get_json_double(sig, "start_time_s", &out->start_time_s)) {
            out->start_time_s = 0.0;
        }
        const bool has_repeat = get_json_double(sig, "repeat_interval_s", &out->repeat_interval_s);
        if (out->loop && has_repeat) {
            /* The two timing models cannot be mixed silently: a looping signal has no
             * silent gap, so a repeat interval would be dead configuration. */
            json_decref(root);
            snprintf(error, error_size, "loop_repeat_conflict");
            return false;
        }
        if (!out->loop && !has_repeat) {
            json_decref(root);
            snprintf(error, error_size, "signal_invalid");
            return false;
        }
        json_t *modulation = json_object_get(sig, "modulation");
        if (json_is_string(modulation)) {
            sim_strlcpy(out->modulation_name, json_string_value(modulation), sizeof(out->modulation_name));
        } else {
            sim_strlcpy(out->modulation_name, "iq", sizeof(out->modulation_name));
        }
        if (!parse_modulation(out->modulation_name, &out->modulation)) {
            json_decref(root);
            snprintf(error, error_size, "signal_modulation_invalid");
            return false;
        }
        if (!get_json_double(sig, "fm_deviation_hz", &out->fm_deviation_hz)) {
            out->fm_deviation_hz = 75000.0;
        }
        if (!get_json_double(sig, "am_depth", &out->am_depth)) {
            out->am_depth = 0.8;
        }
    }

    json_decref(root);
    snprintf(error, error_size, "ok");
    return true;
}

const scenario_source_t *scenario_find_source(const scenario_t *scenario, const char *source_id)
{
    for (size_t i = 0; i < scenario->source_count; i++) {
        if (strcmp(scenario->sources[i].id, source_id) == 0) {
            return &scenario->sources[i];
        }
    }
    return NULL;
}

/* Resolve a relative asset path against base_dir so a scenario works regardless of the process
 * working directory. Absolute paths and an empty/"." base_dir are left as-is. */
static void resolve_asset_path(char *file, size_t file_size, const char *base_dir)
{
    if (base_dir == NULL || base_dir[0] == '\0' || strcmp(base_dir, ".") == 0) {
        return;
    }
    if (file[0] == '/') {
        return;
    }
    char joined[SIM_MAX_PATH];
    const int written = snprintf(joined, sizeof(joined), "%s/%s", base_dir, file);
    if (written > 0 && (size_t)written < sizeof(joined)) {
        sim_strlcpy(file, joined, file_size);
    }
}

/* Non-fatal sanity warnings (stderr): they never fail validation, so existing scenarios keep
 * loading, but they flag likely mistakes such as a declared bandwidth that does not match the
 * source, WBFM narrower than Carson's rule, or a zero AM depth (a bare carrier). */
static void scenario_warn_signal_bandwidth(const scenario_signal_t *signal, const scenario_source_t *source)
{
    if (source->source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
        const double audio_bw = (double)source->sample_rate_hz / 2.0;
        if (signal->modulation == SCENARIO_MODULATION_WBFM) {
            const double carson = 2.0 * (signal->fm_deviation_hz + audio_bw);
            if ((double)signal->bandwidth_hz < carson) {
                fprintf(stderr,
                        "warning: signal %s bandwidth %u Hz is below Carson's rule (~%.0f Hz) for the configured deviation\n",
                        signal->signal_id, signal->bandwidth_hz, carson);
            }
        } else if (signal->modulation == SCENARIO_MODULATION_AM && signal->am_depth == 0.0) {
            fprintf(stderr, "warning: signal %s is AM with am_depth 0 (unmodulated carrier)\n", signal->signal_id);
        }
    } else if (source->source_kind == SCENARIO_SOURCE_IQ_FILE) {
        if (signal->bandwidth_hz > source->bandwidth_hz && source->bandwidth_hz > 0U) {
            fprintf(stderr,
                    "warning: signal %s bandwidth %u Hz exceeds source %s bandwidth %u Hz\n",
                    signal->signal_id, signal->bandwidth_hz, source->id, source->bandwidth_hz);
        }
    }
}

bool scenario_validate(scenario_t *scenario, const char *base_dir, char *error, size_t error_size)
{
    if (scenario->noise_floor.enabled) {
        const double level = scenario->noise_floor.use_density ? scenario->noise_floor.power_dbm_per_hz
                                                               : scenario->noise_floor.power_dbm;
        if (!isfinite(level)) {
            snprintf(error, error_size, "noise_floor_invalid");
            return false;
        }
    }
    for (size_t i = 0; i < scenario->source_count; i++) {
        scenario_source_t *source = &scenario->sources[i];
        for (size_t j = i + 1; j < scenario->source_count; j++) {
            if (strcmp(source->id, scenario->sources[j].id) == 0) {
                snprintf(error, error_size, "duplicate_source_id");
                return false;
            }
        }
        if (source->sample_rate_hz == 0) {
            snprintf(error, error_size, "source_unsupported");
            return false;
        }
        resolve_asset_path(source->file, sizeof(source->file), base_dir);
        if (source->source_kind == SCENARIO_SOURCE_IQ_FILE) {
            if (strcmp(source->format, "ci16") != 0 || strcmp(source->byte_order, "little_endian") != 0 ||
                strcmp(source->iq_layout, "interleaved_iq") != 0) {
                snprintf(error, error_size, "source_unsupported");
                return false;
            }
            iq_file_reader_t reader;
            if (!iq_file_reader_open(&reader, source->file, source->sample_count, error, error_size)) {
                return false;
            }
            source->sample_count = reader.sample_count;
            iq_file_reader_close(&reader);
        } else if (source->source_kind == SCENARIO_SOURCE_AUDIO_FILE) {
            if (strcmp(source->format, "wav") != 0) {
                snprintf(error, error_size, "source_unsupported");
                return false;
            }
            /* Header-only probe: do not read the samples here, the asset cache loads them once. */
            wav_audio_t info;
            if (!wav_reader_probe(source->file, &info, error, error_size)) {
                return false;
            }
            if (source->sample_rate_hz != info.sample_rate_hz) {
                snprintf(error, error_size, "wav_sample_rate_mismatch");
                return false;
            }
            source->sample_count = info.frame_count;
        } else {
            snprintf(error, error_size, "source_unsupported");
            return false;
        }
    }
    for (size_t i = 0; i < scenario->signal_count; i++) {
        const scenario_signal_t *signal = &scenario->signals[i];
        for (size_t j = i + 1; j < scenario->signal_count; j++) {
            if (strcmp(signal->signal_id, scenario->signals[j].signal_id) == 0) {
                snprintf(error, error_size, "duplicate_signal_id");
                return false;
            }
        }
        const scenario_source_t *source = scenario_find_source(scenario, signal->source_reference);
        if (source == NULL || signal->center_frequency_hz > SIM_MAX_RF_HZ || signal->start_time_s < 0.0 ||
            signal->start_time_s >= 86400.0 || (!signal->loop && signal->repeat_interval_s <= 0.0)) {
            snprintf(error, error_size, source == NULL ? "missing_source_reference" : "signal_invalid");
            return false;
        }
        if (!signal->loop) {
            const double duration = (double)source->sample_count / (double)source->sample_rate_hz;
            if (duration > signal->repeat_interval_s) {
                snprintf(error, error_size, "signal_repeat_too_short");
                return false;
            }
        }
        if (signal->replay_mode != SCENARIO_REPLAY_FIXED) {
            if (signal->replay_range_start_hz >= signal->replay_range_stop_hz ||
                signal->replay_range_stop_hz > SIM_MAX_RF_HZ) {
                snprintf(error, error_size, "replay_range_invalid");
                return false;
            }
            /* Range/shift replay bypasses the passband model and streams the file verbatim,
             * which only makes sense for raw IQ recordings. */
            if (source->source_kind != SCENARIO_SOURCE_IQ_FILE || signal->modulation != SCENARIO_MODULATION_IQ) {
                snprintf(error, error_size, "replay_mode_source_mismatch");
                return false;
            }
            if (signal->replay_mode == SCENARIO_REPLAY_SHIFT) {
                if (signal->center_frequency_hz == 0) {
                    snprintf(error, error_size, "replay_shift_missing_center");
                    return false;
                }
                if (signal->center_frequency_hz < signal->replay_range_start_hz ||
                    signal->center_frequency_hz > signal->replay_range_stop_hz) {
                    fprintf(stderr,
                            "warning: signal %s shift center %llu Hz lies outside its frequency range\n",
                            signal->signal_id, (unsigned long long)signal->center_frequency_hz);
                }
            }
        }
        if (signal->loop && source->source_kind != SCENARIO_SOURCE_IQ_FILE) {
            snprintf(error, error_size, "loop_source_unsupported");
            return false;
        }
        if (signal->passthrough) {
            if (signal->replay_mode == SCENARIO_REPLAY_FIXED) {
                snprintf(error, error_size, "passthrough_requires_replay_mode");
                return false;
            }
            if (!signal->loop) {
                snprintf(error, error_size, "passthrough_requires_loop");
                return false;
            }
        }
        if (source->source_kind == SCENARIO_SOURCE_IQ_FILE && signal->modulation != SCENARIO_MODULATION_IQ) {
            snprintf(error, error_size, "signal_modulation_source_mismatch");
            return false;
        }
        if (source->source_kind == SCENARIO_SOURCE_AUDIO_FILE && signal->modulation == SCENARIO_MODULATION_IQ) {
            snprintf(error, error_size, "signal_modulation_source_mismatch");
            return false;
        }
        if (signal->modulation == SCENARIO_MODULATION_WBFM && signal->fm_deviation_hz <= 0.0) {
            snprintf(error, error_size, "signal_modulation_invalid");
            return false;
        }
        if (signal->modulation == SCENARIO_MODULATION_AM && (signal->am_depth < 0.0 || signal->am_depth > 1.0)) {
            snprintf(error, error_size, "signal_modulation_invalid");
            return false;
        }
        scenario_warn_signal_bandwidth(signal, source);
    }
    snprintf(error, error_size, "ok");
    return true;
}
