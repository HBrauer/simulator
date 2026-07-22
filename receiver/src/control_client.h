#ifndef CONTROL_CLIENT_H
#define CONTROL_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* REST client for the simulator's per-receiver channel API. It speaks plain HTTP/1.1
 * over a blocking socket (the API is local and tiny), so the receiver needs no HTTP
 * library dependency. */

#define CONTROL_MAX_CHANNELS 8U
#define CONTROL_MAX_CHANNEL_RATES 128U
#define CONTROL_MAX_NAME 24U
#define CONTROL_MAX_HOST 64U
#define CONTROL_MAX_ERROR 160U

typedef struct {
    uint32_t bandwidth_hz;
    uint32_t sample_rate_hz;
} control_rate_t;

typedef struct {
    uint32_t channel_id;
    bool track_tuner;
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    uint32_t sample_rate_hz;
    uint16_t udp_port;
    bool stream_enabled;
    bool in_frontend_window;
    size_t rate_count;
    control_rate_t rates[CONTROL_MAX_CHANNEL_RATES];
} control_channel_t;

typedef struct {
    char host[CONTROL_MAX_HOST];
    uint16_t port;
    uint32_t receiver_id;
    uint64_t frequency_min_hz;
    uint64_t simulator_frequency_max_hz;
    uint64_t bandwidth_hz;
    /* Current receiver tuner span (fixed-mode center is its midpoint). Populated from the
     * capabilities `tuner` object and refreshed whenever the range is changed. */
    uint64_t tuner_start_hz;
    uint64_t tuner_stop_hz;
    double scan_rate_hz_per_s;
    char udp_output_host[CONTROL_MAX_HOST];
    /* Interface the simulator sends multicast out of; the receiver joins the group on the same
     * interface so a loopback-pinned wideband stream is not routed over a physical NIC. Empty
     * when the simulator lets the kernel choose (unicast, or multicast with no interface set). */
    char udp_multicast_interface[CONTROL_MAX_HOST];
    size_t channel_count;
    control_channel_t channels[CONTROL_MAX_CHANNELS];
} control_client_t;

/* Fetch capabilities and the channel list from base_url ("http://host:port"). */
bool control_client_init(control_client_t *client, const char *base_url, char *error, size_t error_size);

/* Re-read the channel list (e.g. after external changes). */
bool control_client_refresh(control_client_t *client, char *error, size_t error_size);

/* PUT a channel update; NULL fields are left unchanged. On success the client's
 * cached channel entry is replaced with the server's response. bandwidth_hz and
 * sample_rate_hz are set together (a channel carries its own pair). */
bool control_client_set_channel(control_client_t *client, uint32_t channel_id, const uint64_t *center_frequency_hz, const uint32_t *bandwidth_hz, const uint32_t *sample_rate_hz, char *error, size_t error_size);

/* POST the channel's stream enable flag. */
bool control_client_set_stream(control_client_t *client, uint32_t channel_id, bool enabled, char *error, size_t error_size);

/* POST a new receiver tuner range (moves the front-end window; tuner-tracking channels follow).
 * On success the client's cached tuner_start_hz/tuner_stop_hz are updated from the response. */
bool control_client_set_frequency_range(control_client_t *client, uint64_t start_hz, uint64_t stop_hz, char *error, size_t error_size);

#endif
