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

static json_t *receiver_json(const receiver_config_t *r, uint64_t scenario_time_ns, bool include_ddc)
{
    const char *mode = receiver_effective_mode(r) == RECEIVER_MODE_FIXED ? "fixed" : "scan";
    json_t *root = json_pack(
        "{s:i,s:s,s:I,s:I,s:I,s:f,s:f,s:I,s:s,s:{s:{s:i}},s:b}",
        "receiver_id", (int)r->id,
        "effective_mode", mode,
        "frequency_start_hz", (json_int_t)r->frequency_start_hz,
        "frequency_stop_hz", (json_int_t)r->frequency_stop_hz,
        "center_frequency_hz", (json_int_t)receiver_center_frequency_hz(r, scenario_time_ns),
        "scan_rate_hz_per_s", r->scan_rate_hz_per_s,
        "output_scale", r->output_scale,
        "bandwidth_hz", (json_int_t)r->bandwidth_hz,
        "udp_output_host", r->udp_output_host,
        "udp_outputs", "iq_80mhz", "port", (int)r->udp_80mhz_output.port,
        "streams_active", 1
    );
    if (include_ddc) {
        json_t *arr = json_array();
        for (size_t i = 0; i < SIM_DDC_COUNT; i++) {
            json_array_append_new(arr, json_pack(
                "{s:i,s:I,s:i,s:i,s:f,s:{s:i}}",
                "ddc_id", (int)r->ddc[i].id,
                "center_frequency_hz", (json_int_t)r->ddc[i].center_frequency_hz,
                "bandwidth_hz", (int)r->ddc[i].bandwidth_hz,
                "sample_rate_hz", (int)r->ddc[i].sample_rate_hz,
                "output_scale", r->ddc[i].output_scale,
                "udp_output", "port", (int)r->ddc[i].udp_output.port
            ));
        }
        json_object_set_new(root, "ddc", arr);
        json_object_set_new(root, "sample_rate_hz", json_integer(SIM_RECEIVER_SAMPLE_RATE_HZ));
        json_object_set_new(root, "iq_output_format", json_string("ci16"));
    }
    return root;
}

static json_t *stream_metrics_json(const receiver_metrics_t *metrics)
{
    json_t *streams = json_array();
    for (size_t i = 0; i < 1 + SIM_DDC_COUNT; i++) {
        const stream_metrics_t *stream = &metrics->streams[i];
        json_array_append_new(streams, json_pack(
            "{s:s,s:i,s:b,s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
            "stream_type", i == 0 ? "iq_80mhz" : "ddc",
            "stream_id", i == 0 ? -1 : (int)i - 1,
            "active", atomic_load(&stream->active),
            "samples_rendered", (json_int_t)atomic_load(&stream->samples_rendered),
            "udp_packets_sent", (json_int_t)atomic_load(&stream->udp_packets_sent),
            "udp_bytes_sent", (json_int_t)atomic_load(&stream->udp_bytes_sent),
            "udp_send_errors", (json_int_t)atomic_load(&stream->udp_send_errors),
            "ringbuffer_overruns", (json_int_t)atomic_load(&stream->ringbuffer_overruns),
            "ringbuffer_underruns", (json_int_t)atomic_load(&stream->ringbuffer_underruns),
            "samples_dropped", (json_int_t)atomic_load(&stream->samples_dropped)
        ));
    }
    return streams;
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
        json_t *response = json_pack(
            "{s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:I}",
            "samples_rendered", (json_int_t)atomic_load(&ctx->metrics->samples_rendered),
            "udp_packets_sent", (json_int_t)atomic_load(&ctx->metrics->udp_packets_sent),
            "udp_bytes_sent", (json_int_t)atomic_load(&ctx->metrics->udp_bytes_sent),
            "udp_send_errors", (json_int_t)atomic_load(&ctx->metrics->udp_send_errors),
            "active_streams", (json_int_t)atomic_load(&ctx->metrics->active_streams),
            "ringbuffer_overruns", (json_int_t)atomic_load(&ctx->metrics->ringbuffer_overruns),
            "ringbuffer_underruns", (json_int_t)atomic_load(&ctx->metrics->ringbuffer_underruns),
            "samples_dropped", (json_int_t)atomic_load(&ctx->metrics->samples_dropped)
        );
        json_object_set_new(response, "streams", stream_metrics_json(ctx->metrics));
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/config") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = receiver_json(r, scenario_time_ns, true);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/status") == 0) {
        pthread_mutex_lock(ctx->receiver_lock);
        json_t *response = receiver_json(r, scenario_time_ns, false);
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
        *r = updated;
        json_t *response = receiver_json(r, scenario_time_ns, false);
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
        for (size_t i = 0; i < SIM_DDC_COUNT; i++) {
            r->ddc[i].output_scale = output_scale;
        }
        json_t *response = receiver_json(r, scenario_time_ns, true);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    unsigned ddc_id = 99;
    if (sscanf(url, "/api/v1/ddc/%u/status", &ddc_id) == 1 && strcmp(method, "GET") == 0) {
        if (ddc_id >= SIM_DDC_COUNT) {
            SEND_JSON_AND_FREE(MHD_HTTP_NOT_FOUND, error_body("invalid_ddc_id", "ddc_id must be 0..3"));
        }
        pthread_mutex_lock(ctx->receiver_lock);
        const ddc_config_t *d = &r->ddc[ddc_id];
        const bool in_window = receiver_ddc_in_window(r, d, scenario_time_ns);
        json_t *response = json_pack("{s:i,s:i,s:I,s:i,s:i,s:f,s:b,s:{s:i}}", "receiver_id", (int)r->id, "ddc_id", (int)d->id, "center_frequency_hz", (json_int_t)d->center_frequency_hz, "bandwidth_hz", (int)d->bandwidth_hz, "sample_rate_hz", (int)d->sample_rate_hz, "output_scale", d->output_scale, "in_receiver_window", in_window, "udp_output", "port", (int)d->udp_output.port);
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, response);
    }
    if (sscanf(url, "/api/v1/ddc/%u/configure", &ddc_id) == 1 && strcmp(method, "POST") == 0) {
        if (ddc_id >= SIM_DDC_COUNT) {
            SEND_JSON_AND_FREE(MHD_HTTP_NOT_FOUND, error_body("invalid_ddc_id", "ddc_id must be 0..3"));
        }
        json_error_t json_error;
        json_t *request = json_loads(request_body->data, 0, &json_error);
        if (request == NULL) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        pthread_mutex_lock(ctx->receiver_lock);
        uint64_t center = r->ddc[ddc_id].center_frequency_hz;
        double output_scale = r->ddc[ddc_id].output_scale;
        pthread_mutex_unlock(ctx->receiver_lock);
        const bool has_center = json_object_get(request, "center_frequency_hz") != NULL;
        const bool has_scale = json_object_get(request, "output_scale") != NULL;
        if ((!has_center && !has_scale) ||
            (has_center && !json_get_u64(request, "center_frequency_hz", &center)) ||
            (has_scale && !json_get_double(request, "output_scale", &output_scale))) {
            json_decref(request);
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_request", "center_frequency_hz must be an unsigned integer Hz value and output_scale must be a positive number"));
        }
        json_decref(request);
        if (center > SIM_MAX_RF_HZ) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_frequency", "center_frequency_hz must be between 0 and 40000000000"));
        }
        if (output_scale <= 0.0) {
            SEND_JSON_AND_FREE(MHD_HTTP_BAD_REQUEST, error_body("invalid_output_scale", "output_scale must be greater than zero"));
        }
        pthread_mutex_lock(ctx->receiver_lock);
        r->ddc[ddc_id].center_frequency_hz = center;
        r->ddc[ddc_id].output_scale = output_scale;
        pthread_mutex_unlock(ctx->receiver_lock);
        SEND_JSON_AND_FREE(MHD_HTTP_OK, json_pack("{s:s}", "status", "ok"));
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
