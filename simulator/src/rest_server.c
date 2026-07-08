#include "rest_server.h"
#include "receiver.h"

#include <arpa/inet.h>
#include <jansson.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct rest_server {
    struct MHD_Daemon *daemon;
    rest_context_t context;
    struct sockaddr_in bind_addr;
    time_t started_at;
    uint64_t started_at_ns;
};

typedef struct {
    char data[4096];
    size_t len;
    bool too_large;
} request_body_t;

static enum MHD_Result send_json(struct MHD_Connection *connection, unsigned int status, json_t *body)
{
    char *payload = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (payload == NULL) {
        return MHD_NO;
    }
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(payload), payload, MHD_RESPMEM_MUST_FREE);
    if (response == NULL) {
        free(payload);
        return MHD_NO;
    }
    MHD_add_response_header(response, "Content-Type", "application/json");
    const enum MHD_Result ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static json_t *error_body(const char *code, const char *message)
{
    return json_pack("{s:{s:s,s:s}}", "error", "code", code, "message", message);
}

static bool json_get_u64(json_t *object, const char *key, uint64_t *out)
{
    json_t *value = json_object_get(object, key);
    if (!json_is_integer(value)) {
        return false;
    }
    const json_int_t parsed = json_integer_value(value);
    if (parsed < 0) {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

static bool json_get_optional_double(json_t *object, const char *key, double default_value, double *out)
{
    json_t *value = json_object_get(object, key);
    if (value == NULL) {
        *out = default_value;
        return true;
    }
    if (!json_is_number(value)) {
        return false;
    }
    *out = json_number_value(value);
    return true;
}

static bool json_get_double(json_t *object, const char *key, double *out)
{
    json_t *value = json_object_get(object, key);
    if (!json_is_number(value)) {
        return false;
    }
    *out = json_number_value(value);
    return true;
}

static bool json_get_bool_value(json_t *object, const char *key, bool *out)
{
    json_t *value = json_object_get(object, key);
    if (!json_is_boolean(value)) {
        return false;
    }
    *out = json_is_true(value);
    return true;
}

/* Match /api/v1/channels/{id} (suffix "") or /api/v1/channels/{id}/stream (suffix "stream"). */
static bool parse_channel_path(const char *url, const char *suffix, unsigned *channel_id)
{
    int parsed_chars = 0;
    if (sscanf(url, "/api/v1/channels/%u%n", channel_id, &parsed_chars) != 1 || parsed_chars <= 0) {
        return false;
    }
    const char *rest = url + parsed_chars;
    if (suffix[0] == '\0') {
        return rest[0] == '\0';
    }
    return rest[0] == '/' && strcmp(rest + 1, suffix) == 0;
}

static json_t *profiles_json(const receiver_config_t *r)
{
    json_t *profiles = json_array();
    for (size_t i = 0; i < r->profile_count; i++) {
        json_array_append_new(profiles, json_pack(
            "{s:i,s:i,s:s}",
            "bandwidth_hz", (int)r->profiles[i].bandwidth_hz,
            "sample_rate_hz", (int)r->profiles[i].sample_rate_hz,
            "name", r->profiles[i].name
        ));
    }
    return profiles;
}

static json_t *channel_json(const receiver_config_t *r, const channel_config_t *channel, const receiver_metrics_t *metrics, uint64_t scenario_time_ns)
{
    const channel_profile_t *profile = receiver_find_profile(r, channel->bandwidth_hz);
    return json_pack(
        "{s:i,s:b,s:I,s:I,s:i,s:i,s:s,s:b,s:b,s:b,s:i,s:f,s:f}",
        "channel_id", (int)channel->id,
        "track_tuner", channel->track_tuner,
        "center_frequency_hz", (json_int_t)receiver_channel_center_hz(r, channel, scenario_time_ns),
        "configured_center_frequency_hz", (json_int_t)channel->center_frequency_hz,
        "bandwidth_hz", (int)channel->bandwidth_hz,
        "sample_rate_hz", (int)channel->sample_rate_hz,
        "profile", profile != NULL ? profile->name : "",
        "in_frontend_window", receiver_channel_in_window(r, channel, scenario_time_ns),
        "stream_enabled", channel->stream_enabled,
        "active", metrics != NULL ? atomic_load(&metrics->streams[channel->id].active) : false,
        "udp_port", (int)channel->udp_output.port,
        "output_scale", channel->output_scale,
        "rf_reference_power_dbm", channel->rf_reference_power_dbm
    );
}

static json_t *channels_json(const receiver_config_t *r, const receiver_metrics_t *metrics, uint64_t scenario_time_ns)
{
    json_t *channels = json_array();
    for (size_t i = 0; i < r->channel_count; i++) {
        json_array_append_new(channels, channel_json(r, &r->channels[i], metrics, scenario_time_ns));
    }
    return channels;
}

static json_t *receiver_json(const receiver_config_t *r, const receiver_metrics_t *metrics, uint64_t scenario_time_ns, bool include_channels)
{
    const char *mode = receiver_effective_mode(r) == RECEIVER_MODE_FIXED ? "fixed" : "scan";
    json_t *root = json_pack(
        "{s:i,s:s,s:I,s:I,s:I,s:I,s:f,s:f,s:f,s:s,s:s,s:i,s:I}",
        "receiver_id", (int)r->id,
        "effective_mode", mode,
        "frequency_start_hz", (json_int_t)r->frequency_start_hz,
        "frequency_stop_hz", (json_int_t)r->frequency_stop_hz,
        "center_frequency_hz", (json_int_t)receiver_center_frequency_hz(r, scenario_time_ns),
        "frontend_bandwidth_hz", (json_int_t)r->frontend_bandwidth_hz,
        "scan_rate_hz_per_s", r->scan_rate_hz_per_s,
        "output_scale", r->output_scale,
        "rf_reference_power_dbm", r->rf_reference_power_dbm,
        "udp_output_host", r->udp_output_host,
        "udp_multicast_interface", r->udp_multicast_interface,
        "channel_count", (int)r->channel_count,
        "config_epoch", (json_int_t)r->config_epoch
    );
    if (include_channels) {
        json_object_set_new(root, "profiles", profiles_json(r));
        json_object_set_new(root, "channels", channels_json(r, metrics, scenario_time_ns));
        json_object_set_new(root, "iq_output_format", json_string("vita49_2_ci16"));
    }
    return root;
}

static json_t *capabilities_json(const receiver_config_t *r, uint64_t scenario_time_ns)
{
    const char *mode = receiver_effective_mode(r) == RECEIVER_MODE_FIXED ? "fixed" : "scan";
    json_t *root = json_pack(
        "{s:i,s:I,s:I,s:I,s:{s:I,s:I,s:s,s:f},s:i,s:s,s:s,s:I}",
        "receiver_id", (int)r->id,
        "frequency_min_hz", (json_int_t)0,
        "frequency_max_hz", (json_int_t)SIM_MAX_RF_HZ,
        "frontend_bandwidth_hz", (json_int_t)r->frontend_bandwidth_hz,
        "tuner",
        "frequency_start_hz", (json_int_t)r->frequency_start_hz,
        "frequency_stop_hz", (json_int_t)r->frequency_stop_hz,
        "mode", mode,
        "scan_rate_hz_per_s", r->scan_rate_hz_per_s,
        "channel_count", (int)r->channel_count,
        "iq_format", "vita49_2_ci16",
        "udp_output_host", r->udp_output_host,
        "config_epoch", (json_int_t)r->config_epoch
    );
    (void)scenario_time_ns;
    json_object_set_new(root, "profiles", profiles_json(r));
    return root;
}

static uint64_t monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double average_rate(uint64_t samples, uint64_t started_at_ns)
{
    const uint64_t now_ns = monotonic_now_ns();
    if (now_ns <= started_at_ns) {
        return 0.0;
    }
    const double elapsed = (double)(now_ns - started_at_ns) / 1000000000.0;
    return elapsed > 0.0 ? (double)samples / elapsed : 0.0;
}

static json_t *stream_metrics_json(const receiver_config_t *receiver, const receiver_metrics_t *metrics, uint64_t started_at_ns)
{
    json_t *streams = json_array();
    for (size_t i = 0; i < receiver->channel_count; i++) {
        const stream_metrics_t *stream = &metrics->streams[i];
        const uint64_t samples_sent = atomic_load(&stream->samples_sent);
        json_array_append_new(streams, json_pack(
            "{s:s,s:i,s:i,s:b,s:I,s:I,s:f,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
            "stream_type", "channel",
            "stream_id", (int)i,
            "sample_rate_hz", (int)receiver->channels[i].sample_rate_hz,
            "active", atomic_load(&stream->active),
            "samples_rendered", (json_int_t)atomic_load(&stream->samples_rendered),
            "samples_sent", (json_int_t)samples_sent,
            "actual_sample_rate_sps", average_rate(samples_sent, started_at_ns),
            "samples_missed", (json_int_t)atomic_load(&stream->samples_missed),
            "samples_late", (json_int_t)atomic_load(&stream->samples_late),
            "samples_send_dropped", (json_int_t)atomic_load(&stream->samples_send_dropped),
            "udp_packets_sent", (json_int_t)atomic_load(&stream->udp_packets_sent),
            "udp_bytes_sent", (json_int_t)atomic_load(&stream->udp_bytes_sent),
            "udp_send_errors", (json_int_t)atomic_load(&stream->udp_send_errors),
            "udp_send_would_block", (json_int_t)atomic_load(&stream->udp_send_would_block),
            "udp_send_no_buffer", (json_int_t)atomic_load(&stream->udp_send_no_buffer),
            "udp_send_other_errors", (json_int_t)atomic_load(&stream->udp_send_other_errors),
            "ringbuffer_overruns", (json_int_t)atomic_load(&stream->ringbuffer_overruns),
            "ringbuffer_underruns", (json_int_t)atomic_load(&stream->ringbuffer_underruns),
            "samples_dropped", (json_int_t)atomic_load(&stream->samples_dropped),
            "worker_errors", (json_int_t)atomic_load(&stream->worker_errors)
        ));
    }
    return streams;
}

static json_t *stream_status_json(const receiver_config_t *receiver, const receiver_metrics_t *metrics, uint64_t scenario_time_ns)
{
    json_t *streams = json_array();
    for (size_t i = 0; i < receiver->channel_count; i++) {
        const channel_config_t *channel = &receiver->channels[i];
        const stream_metrics_t *stream = &metrics->streams[i];
        json_array_append_new(streams, json_pack(
            "{s:s,s:i,s:i,s:i,s:i,s:I,s:b,s:f,s:f,s:b,s:b,s:b,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
            "stream_type", "channel",
            "stream_id", (int)i,
            "udp_port", (int)channel->udp_output.port,
            "sample_rate_hz", (int)channel->sample_rate_hz,
            "bandwidth_hz", (int)channel->bandwidth_hz,
            "center_frequency_hz", (json_int_t)receiver_channel_center_hz(receiver, channel, scenario_time_ns),
            "track_tuner", channel->track_tuner,
            "output_scale", channel->output_scale,
            "rf_reference_power_dbm", channel->rf_reference_power_dbm,
            "in_frontend_window", receiver_channel_in_window(receiver, channel, scenario_time_ns),
            "enabled", channel->stream_enabled,
            "active", atomic_load(&stream->active),
            "samples_rendered", (json_int_t)atomic_load(&stream->samples_rendered),
            "samples_sent", (json_int_t)atomic_load(&stream->samples_sent),
            "samples_missed", (json_int_t)atomic_load(&stream->samples_missed),
            "samples_late", (json_int_t)atomic_load(&stream->samples_late),
            "samples_send_dropped", (json_int_t)atomic_load(&stream->samples_send_dropped),
            "udp_packets_sent", (json_int_t)atomic_load(&stream->udp_packets_sent),
            "udp_send_would_block", (json_int_t)atomic_load(&stream->udp_send_would_block),
            "udp_send_no_buffer", (json_int_t)atomic_load(&stream->udp_send_no_buffer),
            "udp_send_other_errors", (json_int_t)atomic_load(&stream->udp_send_other_errors),
            "ringbuffer_overruns", (json_int_t)atomic_load(&stream->ringbuffer_overruns),
            "ringbuffer_underruns", (json_int_t)atomic_load(&stream->ringbuffer_underruns)
        ));
    }
    return json_pack("{s:i,s:o}", "receiver_id", (int)receiver->id, "streams", streams);
}

/* Message listing the supported profile bandwidths, for unsupported_bandwidth errors. */
static void supported_bandwidths_message(const receiver_config_t *r, char *out, size_t out_size)
{
    size_t used = (size_t)snprintf(out, out_size, "supported bandwidth_hz:");
    for (size_t i = 0; i < r->profile_count && used < out_size; i++) {
        used += (size_t)snprintf(out + used, out_size - used, "%s %u", i == 0 ? "" : ",", r->profiles[i].bandwidth_hz);
    }
}

static enum MHD_Result answer(void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    (void)version;
    rest_server_t *server = cls;
    rest_context_t *ctx = &server->context;
    receiver_config_t *r = ctx->receiver;

    if (*con_cls == NULL) {
        *con_cls = calloc(1, sizeof(request_body_t));
        if (*con_cls == NULL) {
            return MHD_NO;
        }
        return MHD_YES;
    }
    request_body_t *request_body = *con_cls;
    if (*upload_data_size > 0) {
        const size_t upload_size = *upload_data_size;
        const size_t remaining = sizeof(request_body->data) - request_body->len - 1;
        size_t copy_len = upload_size;
        if (upload_size > remaining) {
            copy_len = remaining;
            request_body->too_large = true;
        }
        memcpy(request_body->data + request_body->len, upload_data, copy_len);
        request_body->len += copy_len;
        request_body->data[request_body->len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }
    *con_cls = NULL;

#define SEND_JSON_AND_FREE(status_code, json_body) \
    do { \
        free(request_body); \
        return send_json(connection, (status_code), (json_body)); \
    } while (0)

    const uint64_t scenario_time_ns = timebase_now_ns(ctx->timebase);
    if (request_body->too_large) {
        SEND_JSON_AND_FREE(MHD_HTTP_CONTENT_TOO_LARGE, error_body("request_too_large", "request body exceeds 4095 bytes"));
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/health") == 0) {
        SEND_JSON_AND_FREE(MHD_HTTP_OK, json_pack("{s:s,s:s,s:i}", "status", "ok", "version", ctx->version, "uptime_s", (int)(time(NULL) - server->started_at)));
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/scenario/status") == 0) {
        SEND_JSON_AND_FREE(MHD_HTTP_OK, json_pack(
            "{s:s,s:b,s:I,s:i,s:i}",
            "scenario_id", ctx->scenario->scenario_id,
            "loaded", 1,
            "scenario_time_ns", (json_int_t)scenario_time_ns,
            "source_count", (int)ctx->scenario->source_count,
            "signal_count", (int)ctx->scenario->signal_count
        ));
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/metrics") == 0) {
        const uint64_t samples_sent = atomic_load(&ctx->metrics->samples_sent);
        json_t *response = json_pack(
            "{s:I,s:I,s:f,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
            "samples_rendered", (json_int_t)atomic_load(&ctx->metrics->samples_rendered),
            "samples_sent", (json_int_t)samples_sent,
            "actual_sample_rate_sps", average_rate(samples_sent, server->started_at_ns),
            "samples_missed", (json_int_t)atomic_load(&ctx->metrics->samples_missed),
            "samples_late", (json_int_t)atomic_load(&ctx->metrics->samples_late),
            "samples_send_dropped", (json_int_t)atomic_load(&ctx->metrics->samples_send_dropped),
            "udp_packets_sent", (json_int_t)atomic_load(&ctx->metrics->udp_packets_sent),
            "udp_bytes_sent", (json_int_t)atomic_load(&ctx->metrics->udp_bytes_sent),
            "udp_send_errors", (json_int_t)atomic_load(&ctx->metrics->udp_send_errors),
            "udp_send_would_block", (json_int_t)atomic_load(&ctx->metrics->udp_send_would_block),
            "udp_send_no_buffer", (json_int_t)atomic_load(&ctx->metrics->udp_send_no_buffer),
            "udp_send_other_errors", (json_int_t)atomic_load(&ctx->metrics->udp_send_other_errors),
            "active_streams", (json_int_t)atomic_load(&ctx->metrics->active_streams),
            "ringbuffer_overruns", (json_int_t)atomic_load(&ctx->metrics->ringbuffer_overruns),
            "ringbuffer_underruns", (json_int_t)atomic_load(&ctx->metrics->ringbuffer_underruns),
            "samples_dropped", (json_int_t)atomic_load(&ctx->metrics->samples_dropped),
            "worker_errors", (json_int_t)atomic_load(&ctx->metrics->worker_errors)
        );
        pthread_mutex_lock(ctx->receiver_lock);
        json_object_set_new(response, "streams", stream_metrics_json(r, ctx->metrics, server->started_at_ns));
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/streams") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = stream_status_json(r, ctx->metrics, scenario_time_ns);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/capabilities") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = capabilities_json(r, scenario_time_ns);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/channels") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = channels_json(r, ctx->metrics, scenario_time_ns);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/config") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = receiver_json(r, ctx->metrics, scenario_time_ns, true);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/status") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = receiver_json(r, ctx->metrics, scenario_time_ns, false);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/v1/frequency-range") == 0) {
        json_error_t json_error;
        json_t *request = json_loads(request_body->data, 0, &json_error);
        if (request == NULL) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        uint64_t start = 0;
        uint64_t stop = 0;
        double scan_rate = r->scan_rate_hz_per_s;
        if (!json_get_u64(request, "frequency_start_hz", &start) ||
            !json_get_u64(request, "frequency_stop_hz", &stop) ||
            !json_get_optional_double(request, "scan_rate_hz_per_s", r->scan_rate_hz_per_s, &scan_rate)) {
            json_decref(request);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_request", "frequency_start_hz and frequency_stop_hz must be unsigned integer Hz values"));
        }
        json_decref(request);
        pthread_mutex_lock(ctx->receiver_lock);
        receiver_config_t updated = *r;
        updated.frequency_start_hz = start;
        updated.frequency_stop_hz = stop;
        updated.scan_rate_hz_per_s = scan_rate;
        char error[128];
        if (!receiver_validate(&updated, error, sizeof(error))) {
            pthread_mutex_unlock(ctx->receiver_lock);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body(error, "invalid frequency range"));
        }
        updated.config_epoch = r->config_epoch + 1U;
        *r = updated;
        json_t *response = receiver_json(r, ctx->metrics, scenario_time_ns, false);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/v1/output-scale") == 0) {
        json_error_t json_error;
        json_t *request = json_loads(request_body->data, 0, &json_error);
        if (request == NULL) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        double output_scale = 0.0;
        if (!json_get_double(request, "output_scale", &output_scale)) {
            json_decref(request);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_request", "output_scale must be a positive number"));
        }
        json_decref(request);
        if (output_scale <= 0.0) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_output_scale", "output_scale must be greater than zero"));
        }
        pthread_mutex_lock(ctx->receiver_lock);
        r->output_scale = output_scale;
        for (size_t i = 0; i < r->channel_count; i++) {
            r->channels[i].output_scale = output_scale;
        }
        r->config_epoch++;
        json_t *response = receiver_json(r, ctx->metrics, scenario_time_ns, true);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    unsigned channel_id = 0;
    if (parse_channel_path(url, "", &channel_id) && strcmp(method, "GET") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        if (channel_id >= r->channel_count) {
            pthread_mutex_unlock(ctx->receiver_lock);
            SEND_JSON_AND_FREE(MHD_HTTP_NOT_FOUND, error_body("invalid_channel_id", "no such channel"));
        }
        json_t *response = channel_json(r, &r->channels[channel_id], ctx->metrics, scenario_time_ns);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (parse_channel_path(url, "stream", &channel_id) && strcmp(method, "POST") == 0) {
        json_error_t json_error;
        json_t *request = json_loads(request_body->data, 0, &json_error);
        if (request == NULL) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        bool enabled = false;
        if (!json_get_bool_value(request, "enabled", &enabled)) {
            json_decref(request);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_request", "enabled must be a boolean"));
        }
        json_decref(request);
        pthread_mutex_lock(ctx->receiver_lock);
        if (channel_id >= r->channel_count) {
            pthread_mutex_unlock(ctx->receiver_lock);
            SEND_JSON_AND_FREE(MHD_HTTP_NOT_FOUND, error_body("invalid_channel_id", "no such channel"));
        }
        r->channels[channel_id].stream_enabled = enabled;
        r->config_epoch++;
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, json_pack("{s:s,s:s,s:i,s:b}", "status", "ok", "stream_type", "channel", "stream_id", (int)channel_id, "enabled", enabled));
    }
    if (parse_channel_path(url, "", &channel_id) && strcmp(method, "PUT") == 0) {
        json_error_t json_error;
        json_t *request = json_loads(request_body->data, 0, &json_error);
        if (request == NULL) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        const bool has_center = json_object_get(request, "center_frequency_hz") != NULL;
        const bool has_bandwidth = json_object_get(request, "bandwidth_hz") != NULL;
        const bool has_track = json_object_get(request, "track_tuner") != NULL;
        const bool has_scale = json_object_get(request, "output_scale") != NULL;
        uint64_t center = 0;
        uint64_t bandwidth = 0;
        bool track_tuner = false;
        double output_scale = 0.0;
        if ((!has_center && !has_bandwidth && !has_track && !has_scale) ||
            (has_center && !json_get_u64(request, "center_frequency_hz", &center)) ||
            (has_bandwidth && !json_get_u64(request, "bandwidth_hz", &bandwidth)) ||
            (has_track && !json_get_bool_value(request, "track_tuner", &track_tuner)) ||
            (has_scale && !json_get_double(request, "output_scale", &output_scale))) {
            json_decref(request);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_request", "expected center_frequency_hz (unsigned Hz), bandwidth_hz (unsigned Hz), track_tuner (boolean), output_scale (positive number)"));
        }
        json_decref(request);
        if (has_scale && output_scale <= 0.0) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_output_scale", "output_scale must be greater than zero"));
        }

        pthread_mutex_lock(ctx->receiver_lock);
        if (channel_id >= r->channel_count) {
            pthread_mutex_unlock(ctx->receiver_lock);
            SEND_JSON_AND_FREE(MHD_HTTP_NOT_FOUND, error_body("invalid_channel_id", "no such channel"));
        }
        receiver_config_t updated = *r;
        channel_config_t *channel = &updated.channels[channel_id];
        if (has_track) {
            channel->track_tuner = track_tuner;
        }
        if (has_center) {
            if (channel->track_tuner) {
                pthread_mutex_unlock(ctx->receiver_lock);
                SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_request", "center_frequency_hz requires track_tuner false; this channel follows the receiver tuner"));
            }
            channel->center_frequency_hz = center;
        }
        if (has_bandwidth) {
            const channel_profile_t *profile = receiver_find_profile(&updated, (uint32_t)bandwidth);
            if (profile == NULL || bandwidth > UINT32_MAX) {
                char message[256];
                supported_bandwidths_message(&updated, message, sizeof(message));
                pthread_mutex_unlock(ctx->receiver_lock);
                SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("unsupported_bandwidth", message));
            }
            channel->bandwidth_hz = profile->bandwidth_hz;
            channel->sample_rate_hz = profile->sample_rate_hz;
        }
        if (has_scale) {
            channel->output_scale = output_scale;
        }
        char error[128];
        if (!receiver_validate(&updated, error, sizeof(error))) {
            pthread_mutex_unlock(ctx->receiver_lock);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body(error, "invalid channel configuration"));
        }
        updated.config_epoch = r->config_epoch + 1U;
        *r = updated;
        json_t *response = channel_json(r, &r->channels[channel_id], ctx->metrics, scenario_time_ns);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }

    SEND_JSON_AND_FREE(MHD_HTTP_NOT_FOUND, error_body("not_found", "unknown endpoint"));

#undef SEND_JSON_AND_FREE
}

bool rest_server_start(rest_server_t **server, const rest_context_t *context)
{
    rest_server_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return false;
    }
    s->context = *context;
    s->started_at = time(NULL);
    s->started_at_ns = monotonic_now_ns();
    memset(&s->bind_addr, 0, sizeof(s->bind_addr));
    s->bind_addr.sin_family = AF_INET;
    s->bind_addr.sin_port = htons(context->receiver->rest_port);
    if (inet_pton(AF_INET, context->receiver->rest_bind_host, &s->bind_addr.sin_addr) != 1) {
        free(s);
        return false;
    }
    s->daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        context->receiver->rest_port,
        NULL,
        NULL,
        answer,
        s,
        MHD_OPTION_SOCK_ADDR,
        (struct sockaddr *)&s->bind_addr,
        MHD_OPTION_END);
    if (s->daemon == NULL) {
        free(s);
        return false;
    }
    *server = s;
    return true;
}

void rest_server_stop(rest_server_t *server)
{
    if (server != NULL) {
        if (server->daemon != NULL) {
            MHD_stop_daemon(server->daemon);
        }
        free(server);
    }
}
