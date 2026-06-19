#include "rest_server.h"
#include "receiver.h"

#include <jansson.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct rest_server {
    struct MHD_Daemon *daemon;
    rest_context_t context;
    time_t started_at;
};

static int send_json(struct MHD_Connection *connection, unsigned int status, json_t *body)
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
    const int ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static json_t *error_body(const char *code, const char *message)
{
    return json_pack("{s:{s:s,s:s}}", "error", "code", code, "message", message);
}

static json_t *receiver_json(const receiver_config_t *r, uint64_t scenario_time_ns, bool include_ddc)
{
    const char *mode = receiver_effective_mode(r) == RECEIVER_MODE_FIXED ? "fixed" : "scan";
    json_t *root = json_pack(
        "{s:i,s:s,s:I,s:I,s:I,s:f,s:I,s:s,s:{s:{s:i}},s:b}",
        "receiver_id", (int)r->id,
        "effective_mode", mode,
        "frequency_start_hz", (json_int_t)r->frequency_start_hz,
        "frequency_stop_hz", (json_int_t)r->frequency_stop_hz,
        "center_frequency_hz", (json_int_t)receiver_center_frequency_hz(r, scenario_time_ns),
        "scan_rate_hz_per_s", r->scan_rate_hz_per_s,
        "bandwidth_hz", (json_int_t)r->bandwidth_hz,
        "udp_output_host", r->udp_output_host,
        "udp_outputs", "iq_80mhz", "port", (int)r->udp_80mhz_output.port,
        "streams_active", 1
    );
    if (include_ddc) {
        json_t *arr = json_array();
        for (size_t i = 0; i < SIM_DDC_COUNT; i++) {
            json_array_append_new(arr, json_pack(
                "{s:i,s:I,s:i,s:i,s:{s:i}}",
                "ddc_id", (int)r->ddc[i].id,
                "center_frequency_hz", (json_int_t)r->ddc[i].center_frequency_hz,
                "bandwidth_hz", (int)r->ddc[i].bandwidth_hz,
                "sample_rate_hz", (int)r->ddc[i].sample_rate_hz,
                "udp_output", "port", (int)r->ddc[i].udp_output.port
            ));
        }
        json_object_set_new(root, "ddc", arr);
        json_object_set_new(root, "sample_rate_hz", json_integer(SIM_RECEIVER_SAMPLE_RATE_HZ));
        json_object_set_new(root, "iq_output_format", json_string("ci16"));
    }
    return root;
}

static int answer(void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    (void)version;
    rest_server_t *server = cls;
    rest_context_t *ctx = &server->context;
    receiver_config_t *r = ctx->receiver;

    if (*con_cls == NULL) {
        *con_cls = calloc(1, 4096);
        return MHD_YES;
    }
    char *body = *con_cls;
    if (*upload_data_size > 0) {
        strncat(body, upload_data, 4095 - strlen(body));
        *upload_data_size = 0;
        return MHD_YES;
    }
    free(*con_cls);
    *con_cls = NULL;

    const uint64_t scenario_time_ns = timebase_now_ns(ctx->timebase);
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/health") == 0) {
        return send_json(connection, MHD_HTTP_OK, json_pack("{s:s,s:s,s:i}", "status", "ok", "version", ctx->version, "uptime_s", (int)(time(NULL) - server->started_at)));
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/scenario/status") == 0) {
        return send_json(connection, MHD_HTTP_OK, json_pack(
            "{s:s,s:b,s:I,s:i,s:i}",
            "scenario_id", ctx->scenario->scenario_id,
            "loaded", 1,
            "scenario_time_ns", (json_int_t)scenario_time_ns,
            "source_count", (int)ctx->scenario->source_count,
            "signal_count", (int)ctx->scenario->signal_count
        ));
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/config") == 0) {
        return send_json(connection, MHD_HTTP_OK, receiver_json(r, scenario_time_ns, true));
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/status") == 0) {
        return send_json(connection, MHD_HTTP_OK, receiver_json(r, scenario_time_ns, false));
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/v1/frequency-range") == 0) {
        json_error_t json_error;
        json_t *request = json_loads(body, 0, &json_error);
        if (request == NULL) {
            return send_json(connection, MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        uint64_t start = (uint64_t)json_integer_value(json_object_get(request, "frequency_start_hz"));
        uint64_t stop = (uint64_t)json_integer_value(json_object_get(request, "frequency_stop_hz"));
        double scan_rate = json_number_value(json_object_get(request, "scan_rate_hz_per_s"));
        json_decref(request);
        receiver_config_t updated = *r;
        updated.frequency_start_hz = start;
        updated.frequency_stop_hz = stop;
        updated.scan_rate_hz_per_s = scan_rate;
        char error[128];
        if (!receiver_validate(&updated, error, sizeof(error))) {
            return send_json(connection, MHD_HTTP_BAD_REQUEST, error_body(error, "invalid frequency range"));
        }
        *r = updated;
        return send_json(connection, MHD_HTTP_OK, receiver_json(r, scenario_time_ns, false));
    }
    unsigned ddc_id = 99;
    if (sscanf(url, "/api/v1/ddc/%u/status", &ddc_id) == 1 && strcmp(method, "GET") == 0) {
        if (ddc_id >= SIM_DDC_COUNT) {
            return send_json(connection, MHD_HTTP_NOT_FOUND, error_body("invalid_ddc_id", "ddc_id must be 0..3"));
        }
        const ddc_config_t *d = &r->ddc[ddc_id];
        return send_json(connection, MHD_HTTP_OK, json_pack("{s:i,s:i,s:I,s:i,s:i,s:{s:i}}", "receiver_id", (int)r->id, "ddc_id", (int)d->id, "center_frequency_hz", (json_int_t)d->center_frequency_hz, "bandwidth_hz", (int)d->bandwidth_hz, "sample_rate_hz", (int)d->sample_rate_hz, "udp_output", "port", (int)d->udp_output.port));
    }
    if (sscanf(url, "/api/v1/ddc/%u/configure", &ddc_id) == 1 && strcmp(method, "POST") == 0) {
        if (ddc_id >= SIM_DDC_COUNT) {
            return send_json(connection, MHD_HTTP_NOT_FOUND, error_body("invalid_ddc_id", "ddc_id must be 0..3"));
        }
        json_error_t json_error;
        json_t *request = json_loads(body, 0, &json_error);
        if (request == NULL) {
            return send_json(connection, MHD_HTTP_BAD_REQUEST, error_body("invalid_json", json_error.text));
        }
        uint64_t center = (uint64_t)json_integer_value(json_object_get(request, "center_frequency_hz"));
        json_decref(request);
        if (center > SIM_MAX_RF_HZ) {
            return send_json(connection, MHD_HTTP_BAD_REQUEST, error_body("invalid_frequency", "center_frequency_hz must be between 0 and 40000000000"));
        }
        r->ddc[ddc_id].center_frequency_hz = center;
        return send_json(connection, MHD_HTTP_OK, json_pack("{s:s}", "status", "ok"));
    }

    return send_json(connection, MHD_HTTP_NOT_FOUND, error_body("not_found", "unknown endpoint"));
}

bool rest_server_start(rest_server_t **server, const rest_context_t *context)
{
    rest_server_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return false;
    }
    s->context = *context;
    s->started_at = time(NULL);
    s->daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, context->receiver->rest_port, NULL, NULL, answer, s, MHD_OPTION_END);
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
