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
            if (!get_json_double(noise, "power_dbm", &scenario->noise_floor.power_dbm)) {
                json_decref(root);
                snprintf(error, error_size, "noise_floor_invalid");
                return false;
            }
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
        if (!get_json_string(sig, "signal_id", out->signal_id, sizeof(out->signal_id)) ||
            !get_json_string(sig, "source_reference", out->source_reference, sizeof(out->source_reference)) ||
            !get_json_u64(sig, "center_frequency_hz", &out->center_frequency_hz) ||
            !get_json_u32(sig, "bandwidth_hz", &out->bandwidth_hz) ||
            !get_json_double(sig, "power_dbm", &out->power_dbm) ||
            !get_json_double(sig, "start_time_s", &out->start_time_s) ||
            !get_json_double(sig, "repeat_interval_s", &out->repeat_interval_s)) {
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

bool scenario_validate(scenario_t *scenario, const char *base_dir, char *error, size_t error_size)
{
    (void)base_dir;
    if (scenario->noise_floor.enabled && !isfinite(scenario->noise_floor.power_dbm)) {
        snprintf(error, error_size, "noise_floor_invalid");
        return false;
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
            wav_audio_t audio;
            if (!wav_reader_load_mono_f32(source->file, &audio, error, error_size)) {
                return false;
            }
            if (source->sample_rate_hz != audio.sample_rate_hz) {
                wav_audio_free(&audio);
                snprintf(error, error_size, "wav_sample_rate_mismatch");
                return false;
            }
            source->sample_count = audio.frame_count;
            wav_audio_free(&audio);
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
            signal->start_time_s >= 86400.0 || signal->repeat_interval_s <= 0.0) {
            snprintf(error, error_size, source == NULL ? "missing_source_reference" : "signal_invalid");
            return false;
        }
        const double duration = (double)source->sample_count / (double)source->sample_rate_hz;
        if (duration > signal->repeat_interval_s) {
            snprintf(error, error_size, "signal_repeat_too_short");
            return false;
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
    }
    snprintf(error, error_size, "ok");
    return true;
}
