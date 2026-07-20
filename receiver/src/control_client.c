#include "control_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <jansson.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_TIMEOUT_S 2
#define HTTP_MAX_RESPONSE (256U * 1024U)

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

static bool parse_base_url(const char *base_url, char *host, size_t host_size, uint16_t *port)
{
    const char *p = base_url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *colon = strchr(p, ':');
    if (colon == NULL) {
        return false;
    }
    const size_t host_len = (size_t)(colon - p);
    if (host_len == 0 || host_len >= host_size) {
        return false;
    }
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    char *end = NULL;
    const long parsed = strtol(colon + 1, &end, 10);
    if (parsed <= 0 || parsed > 65535 || (end != NULL && *end != '\0' && *end != '/')) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

/* One HTTP exchange with Connection: close, so the response ends at EOF. Returns the
 * malloc'd body and the HTTP status code. */
static bool http_request(const control_client_t *client, const char *method, const char *path, const char *body, char **response_body, int *status, char *error, size_t error_size)
{
    *response_body = NULL;
    *status = 0;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_error(error, error_size, "socket failed");
        return false;
    }
    const struct timeval timeout = {.tv_sec = HTTP_TIMEOUT_S, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(client->port);
    if (inet_pton(AF_INET, client->host, &addr.sin_addr) != 1) {
        close(fd);
        set_error(error, error_size, "control host must be an IPv4 address");
        return false;
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        set_error(error, error_size, "control connection failed");
        return false;
    }

    const size_t body_len = body != NULL ? strlen(body) : 0;
    char header[512];
    const int header_len = snprintf(header,
                                    sizeof(header),
                                    "%s %s HTTP/1.1\r\n"
                                    "Host: %s:%u\r\n"
                                    "Connection: close\r\n"
                                    "Content-Type: application/json\r\n"
                                    "Content-Length: %zu\r\n"
                                    "\r\n",
                                    method,
                                    path,
                                    client->host,
                                    client->port,
                                    body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header) ||
        send(fd, header, (size_t)header_len, 0) != header_len ||
        (body_len > 0 && send(fd, body, body_len, 0) != (ssize_t)body_len)) {
        close(fd);
        set_error(error, error_size, "control request send failed");
        return false;
    }

    size_t capacity = 8192;
    size_t used = 0;
    char *response = malloc(capacity);
    if (response == NULL) {
        close(fd);
        set_error(error, error_size, "out of memory");
        return false;
    }
    for (;;) {
        if (used + 4096 >= capacity) {
            if (capacity >= HTTP_MAX_RESPONSE) {
                free(response);
                close(fd);
                set_error(error, error_size, "control response too large");
                return false;
            }
            capacity *= 2;
            char *grown = realloc(response, capacity);
            if (grown == NULL) {
                free(response);
                close(fd);
                set_error(error, error_size, "out of memory");
                return false;
            }
            response = grown;
        }
        const ssize_t received = recv(fd, response + used, capacity - used - 1, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(response);
            close(fd);
            set_error(error, error_size, "control response receive failed");
            return false;
        }
        if (received == 0) {
            break;
        }
        used += (size_t)received;
    }
    close(fd);
    response[used] = '\0';

    if (sscanf(response, "HTTP/1.%*c %d", status) != 1) {
        free(response);
        set_error(error, error_size, "malformed HTTP response");
        return false;
    }
    const char *separator = strstr(response, "\r\n\r\n");
    if (separator == NULL) {
        free(response);
        set_error(error, error_size, "malformed HTTP response");
        return false;
    }
    *response_body = strdup(separator + 4);
    free(response);
    if (*response_body == NULL) {
        set_error(error, error_size, "out of memory");
        return false;
    }
    return true;
}

static void set_error_from_api(json_t *root, int status, char *error, size_t error_size)
{
    json_t *message = json_object_get(json_object_get(root, "error"), "message");
    if (json_is_string(message)) {
        set_error(error, error_size, json_string_value(message));
    } else {
        char fallback[64];
        snprintf(fallback, sizeof(fallback), "control request failed (HTTP %d)", status);
        set_error(error, error_size, fallback);
    }
}

static json_t *request_json(const control_client_t *client, const char *method, const char *path, const char *body, char *error, size_t error_size)
{
    char *response_body = NULL;
    int status = 0;
    if (!http_request(client, method, path, body, &response_body, &status, error, error_size)) {
        return NULL;
    }
    json_error_t json_error;
    json_t *root = json_loads(response_body, 0, &json_error);
    free(response_body);
    if (root == NULL) {
        set_error(error, error_size, "control response is not valid JSON");
        return NULL;
    }
    if (status != 200) {
        set_error_from_api(root, status, error, error_size);
        json_decref(root);
        return NULL;
    }
    return root;
}

static uint64_t json_u64(json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_integer(value) && json_integer_value(value) > 0 ? (uint64_t)json_integer_value(value) : 0U;
}

static bool json_flag(json_t *object, const char *key)
{
    return json_is_true(json_object_get(object, key));
}

static bool parse_channel(json_t *entry, control_channel_t *channel)
{
    if (!json_is_object(entry)) {
        return false;
    }
    channel->channel_id = (uint32_t)json_u64(entry, "channel_id");
    channel->track_tuner = json_flag(entry, "track_tuner");
    channel->center_frequency_hz = json_u64(entry, "center_frequency_hz");
    channel->bandwidth_hz = (uint32_t)json_u64(entry, "bandwidth_hz");
    channel->sample_rate_hz = (uint32_t)json_u64(entry, "sample_rate_hz");
    channel->udp_port = (uint16_t)json_u64(entry, "udp_port");
    channel->stream_enabled = json_flag(entry, "stream_enabled");
    channel->in_frontend_window = json_flag(entry, "in_frontend_window");
    channel->rate_count = 0;
    json_t *rates = json_object_get(entry, "rates");
    if (json_is_array(rates)) {
        size_t count = json_array_size(rates);
        if (count > CONTROL_MAX_CHANNEL_RATES) {
            count = CONTROL_MAX_CHANNEL_RATES;
        }
        for (size_t i = 0; i < count; i++) {
            json_t *rate = json_array_get(rates, i);
            channel->rates[i].bandwidth_hz = (uint32_t)json_u64(rate, "bandwidth_hz");
            channel->rates[i].sample_rate_hz = (uint32_t)json_u64(rate, "sample_rate_hz");
        }
        channel->rate_count = count;
    }
    return channel->udp_port != 0 && channel->sample_rate_hz != 0;
}

static bool parse_channels_array(control_client_t *client, json_t *channels, char *error, size_t error_size)
{
    if (!json_is_array(channels) || json_array_size(channels) == 0 ||
        json_array_size(channels) > CONTROL_MAX_CHANNELS) {
        set_error(error, error_size, "unexpected channels response");
        return false;
    }
    client->channel_count = json_array_size(channels);
    for (size_t i = 0; i < client->channel_count; i++) {
        if (!parse_channel(json_array_get(channels, i), &client->channels[i])) {
            set_error(error, error_size, "unexpected channel entry");
            return false;
        }
    }
    return true;
}

bool control_client_refresh(control_client_t *client, char *error, size_t error_size)
{
    json_t *channels = request_json(client, "GET", "/api/v1/channels", NULL, error, error_size);
    if (channels == NULL) {
        return false;
    }
    const bool ok = parse_channels_array(client, channels, error, error_size);
    json_decref(channels);
    return ok;
}

bool control_client_init(control_client_t *client, const char *base_url, char *error, size_t error_size)
{
    memset(client, 0, sizeof(*client));
    if (!parse_base_url(base_url, client->host, sizeof(client->host), &client->port)) {
        set_error(error, error_size, "control url must look like http://host:port");
        return false;
    }

    json_t *capabilities = request_json(client, "GET", "/api/v1/capabilities", NULL, error, error_size);
    if (capabilities == NULL) {
        return false;
    }
    client->receiver_id = (uint32_t)json_u64(capabilities, "receiver_id");
    client->frequency_min_hz = json_u64(capabilities, "frequency_min_hz");
    client->simulator_frequency_max_hz = json_u64(capabilities, "simulator_frequency_max_hz");
    client->bandwidth_hz = json_u64(capabilities, "bandwidth_hz");
    json_t *tuner = json_object_get(capabilities, "tuner");
    if (json_is_object(tuner)) {
        client->tuner_start_hz = json_u64(tuner, "frequency_min_hz");
        client->tuner_stop_hz = json_u64(tuner, "frequency_max_hz");
        json_t *scan_rate = json_object_get(tuner, "scan_rate_hz_per_s");
        if (json_is_number(scan_rate)) {
            client->scan_rate_hz_per_s = json_number_value(scan_rate);
        }
    }
    json_t *udp_host = json_object_get(capabilities, "udp_output_host");
    if (json_is_string(udp_host)) {
        snprintf(client->udp_output_host, sizeof(client->udp_output_host), "%s", json_string_value(udp_host));
    }
    json_decref(capabilities);

    return control_client_refresh(client, error, error_size);
}

bool control_client_set_channel(control_client_t *client, uint32_t channel_id, const uint64_t *center_frequency_hz, const uint32_t *bandwidth_hz, const uint32_t *sample_rate_hz, char *error, size_t error_size)
{
    if (channel_id >= client->channel_count ||
        (center_frequency_hz == NULL && bandwidth_hz == NULL && sample_rate_hz == NULL)) {
        set_error(error, error_size, "invalid channel update");
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/channels/%u", channel_id);
    char body[160];
    size_t n = (size_t)snprintf(body, sizeof(body), "{");
    const char *sep = "";
    if (center_frequency_hz != NULL) {
        n += (size_t)snprintf(body + n, sizeof(body) - n, "%s\"center_frequency_hz\":%llu", sep, (unsigned long long)*center_frequency_hz);
        sep = ",";
    }
    if (bandwidth_hz != NULL) {
        n += (size_t)snprintf(body + n, sizeof(body) - n, "%s\"bandwidth_hz\":%u", sep, *bandwidth_hz);
        sep = ",";
    }
    if (sample_rate_hz != NULL) {
        n += (size_t)snprintf(body + n, sizeof(body) - n, "%s\"sample_rate_hz\":%u", sep, *sample_rate_hz);
        sep = ",";
    }
    snprintf(body + n, sizeof(body) - n, "}");
    json_t *updated = request_json(client, "PUT", path, body, error, error_size);
    if (updated == NULL) {
        return false;
    }
    const bool ok = parse_channel(updated, &client->channels[channel_id]);
    json_decref(updated);
    if (!ok) {
        set_error(error, error_size, "unexpected channel response");
    }
    return ok;
}

bool control_client_set_frequency_range(control_client_t *client, uint64_t start_hz, uint64_t stop_hz, char *error, size_t error_size)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"frequency_min_hz\":%llu,\"frequency_max_hz\":%llu}", (unsigned long long)start_hz, (unsigned long long)stop_hz);
    json_t *response = request_json(client, "POST", "/api/v1/frequency-range", body, error, error_size);
    if (response == NULL) {
        return false;
    }
    const uint64_t new_start = json_u64(response, "frequency_min_hz");
    const uint64_t new_stop = json_u64(response, "frequency_max_hz");
    json_decref(response);
    if (new_start == 0U || new_stop <= new_start) {
        set_error(error, error_size, "unexpected frequency-range response");
        return false;
    }
    client->tuner_start_hz = new_start;
    client->tuner_stop_hz = new_stop;
    return true;
}

bool control_client_set_stream(control_client_t *client, uint32_t channel_id, bool enabled, char *error, size_t error_size)
{
    if (channel_id >= client->channel_count) {
        set_error(error, error_size, "invalid channel id");
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/channels/%u/stream", channel_id);
    json_t *response = request_json(client, "POST", path, enabled ? "{\"enabled\":true}" : "{\"enabled\":false}", error, error_size);
    if (response == NULL) {
        return false;
    }
    json_decref(response);
    client->channels[channel_id].stream_enabled = enabled;
    return true;
}
