#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "control_client.h"
#include "history.h"
#include "ui_text.h"
#include "vita49_rx.h"
#include "waterfall.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#define MAX_PACKET_BYTES 65536U
#define RX_BATCH_SIZE 64U
#define REQUESTED_RCVBUF_BYTES (128 * 1024 * 1024)
#define MIN_RECOMMENDED_RCVBUF_BYTES (16 * 1024 * 1024)
#define DEFAULT_SAMPLE_RATE_HZ 98304000U
#define UI_TOOLBAR_HEIGHT 44
#define UI_SPECTRUM_HEIGHT 128
#define UI_SPECTRUM_MIN_HEIGHT 56
#define UI_BUTTON_SIZE 32
#define UI_BUTTON_MARGIN 8
#define UI_TEXT_SCALE 2
#define UI_CHANNEL_COMBO_WIDTH 76
#define UI_FREQ_FIELD_WIDTH 176
#define UI_BANDWIDTH_COMBO_WIDTH 110
#define UI_DROPDOWN_ITEM_HEIGHT 26
#define UI_DROPDOWN_WIDTH 280
#define UI_FREQ_EDIT_MAX 16
#define AUTO_LEVEL_DYNAMIC_RANGE_DB 150.0f
#define AUTO_LEVEL_HEADROOM_DB 4.0f
#define AUTO_LEVEL_ATTACK 0.35f
#define AUTO_LEVEL_RELEASE 0.03f
#define WATERFALL_EMPTY_DB (-230.0f)

static volatile sig_atomic_t keep_running = 1;

typedef struct {
    const char *host;
    const char *interface_host;
    uint16_t port;
    size_t fft_size;
    size_t rows;
    bool auto_rows;
    uint32_t sample_rate_hz;
    size_t frame_stride_samples;
    double history_seconds;
    uint64_t max_packets;
    uint64_t max_frames;
    int width;
    int height;
    float min_db;
    float max_db;
    bool auto_level;
    bool log_iq_stats;
    bool headless;
    const char *control_url;
    uint32_t channel_id;
} app_config_t;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t payload_samples;
    uint64_t frames;
    uint64_t bad_packets;
    uint64_t context_packets;
    uint64_t sequence_gaps;
    bool have_sequence;
    uint8_t last_sequence;
    uint32_t last_stream_id;
    uint64_t last_timestamp_ns;
    /* Last in-band stream configuration seen in a VITA context packet. */
    uint64_t stream_center_hz;
    uint64_t stream_bandwidth_hz;
    /* Rate measurement window, restarted on channel/rate changes so the title shows the
     * current stream rate instead of a lifetime average. */
    uint64_t rate_window_samples;
    uint64_t rate_window_start_ns;
    /* Settle window after a rate change: in-flight packets rendered at the old rate keep
     * arriving briefly and must not be folded into the first waterfall rows. */
    uint64_t discard_until_ns;
} rx_stats_t;

/* REST control state (--control-url). The channel selection, retunes, and bandwidth
 * changes go through the simulator's channel API; sample-rate changes are then picked
 * up from the in-band VITA context packets. */
typedef struct {
    bool enabled;
    control_client_t client;
    uint32_t channel;
    char multicast_host[CONTROL_MAX_HOST];
    uint64_t last_refresh_attempt_ns; /* throttles cache refreshes on in-band mismatches */
} control_state_t;

typedef struct {
    int16_t *iq;
    size_t collected_samples;
    size_t samples_until_frame;
} frame_sampler_t;

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
    return 0;
}

static bool arg_present(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static uint64_t parse_u64_default(const char *value, uint64_t default_value)
{
    return value == 0 ? default_value : strtoull(value, 0, 10);
}

static float parse_float_default(const char *value, float default_value)
{
    return value == 0 ? default_value : strtof(value, 0);
}

static bool ipv4_is_multicast(struct in_addr addr)
{
    return IN_MULTICAST(ntohl(addr.s_addr));
}

static int spectrum_height_for_window(int height)
{
    const int available = height - UI_TOOLBAR_HEIGHT;
    if (available <= 0) {
        return 0;
    }
    if (available < UI_SPECTRUM_MIN_HEIGHT * 2) {
        return available / 2;
    }
    return available < UI_SPECTRUM_HEIGHT ? available / 2 : UI_SPECTRUM_HEIGHT;
}

static size_t rows_for_window_height(int height)
{
    const int rows = height - UI_TOOLBAR_HEIGHT - spectrum_height_for_window(height);
    return rows > 0 ? (size_t)rows : 1U;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --port PORT [options]\n"
            "\n"
            "Options:\n"
            "  --host HOST                 UDP bind host, default 0.0.0.0\n"
            "  --interface HOST            Multicast receive interface, default 0.0.0.0\n"
            "  --fft-size N                Power-of-two FFT size, default 1024\n"
            "  --rows N                    Override automatic one-row-per-screen-pixel layout\n"
            "  --sample-rate-hz N          Stream sample rate, default 98304000\n"
            "  --max-packets N             Exit after N UDP packets, default unlimited\n"
            "  --max-frames N              Exit after N waterfall frames, default unlimited\n"
            "  --width N                   Window width, default 1200\n"
            "  --height N                  Window height, default 700\n"
            "  --min-db DB                 Manual waterfall floor, default -100\n"
            "  --max-db DB                 Manual waterfall ceiling, default 0\n"
            "  --no-auto-level             Disable dynamic waterfall levels\n"
            "  --log-iq-stats              Log periodic received CI16 payload min/max/nonzero counts\n"
            "  --headless                  Receive/process without opening SDL window\n"
            "  --control-url URL           Simulator REST API, e.g. http://127.0.0.1:8100.\n"
            "                              Discovers port/sample rate and enables channel controls;\n"
            "                              --port and --sample-rate-hz are then not needed.\n"
            "  --channel N                 Initial channel to receive (with --control-url), default 0\n"
            "\n"
            "Controls (with --control-url): an RX field that moves the receiver tuner (span\n"
            "preserved; tuner-tracking channels follow), channel and bandwidth combo boxes, and a\n"
            "channel center-frequency field. Click a field, type MHz, Enter commits, Esc cancels.\n"
            "Keys: PgUp/PgDn or 0-7 select channel, Left/Right retune the channel (Shift = x10),\n"
            "B cycles the bandwidth profile.\n",
            argv0);
}

static bool parse_args(int argc, char **argv, app_config_t *config)
{
    *config = (app_config_t){
        .host = "0.0.0.0",
        .interface_host = "0.0.0.0",
        .port = 0,
        .fft_size = 1024,
        .rows = 0,
        .auto_rows = true,
        .sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ,
        .frame_stride_samples = 0,
        .history_seconds = WATERFALL_HISTORY_DEFAULT_SECONDS,
        .max_packets = 0,
        .max_frames = 0,
        .width = 1200,
        .height = 700,
        .min_db = -100.0f,
        .max_db = 0.0f,
        .auto_level = true,
        .log_iq_stats = false,
        .headless = false,
        .control_url = 0,
        .channel_id = 0,
    };
    const char *host = arg_value(argc, argv, "--host");
    if (host != 0) {
        config->host = host;
    }
    const char *interface_host = arg_value(argc, argv, "--interface");
    if (interface_host != 0) {
        config->interface_host = interface_host;
    }
    config->port = (uint16_t)parse_u64_default(arg_value(argc, argv, "--port"), 0);
    config->fft_size = (size_t)parse_u64_default(arg_value(argc, argv, "--fft-size"), config->fft_size);
    const char *rows_arg = arg_value(argc, argv, "--rows");
    if (rows_arg != 0) {
        config->rows = (size_t)parse_u64_default(rows_arg, 0);
        config->auto_rows = false;
    }
    config->sample_rate_hz = (uint32_t)parse_u64_default(arg_value(argc, argv, "--sample-rate-hz"), config->sample_rate_hz);
    config->max_packets = parse_u64_default(arg_value(argc, argv, "--max-packets"), 0);
    config->max_frames = parse_u64_default(arg_value(argc, argv, "--max-frames"), 0);
    config->width = (int)parse_u64_default(arg_value(argc, argv, "--width"), (uint64_t)config->width);
    config->height = (int)parse_u64_default(arg_value(argc, argv, "--height"), (uint64_t)config->height);
    if (config->auto_rows) {
        config->rows = rows_for_window_height(config->height);
    }
    config->min_db = parse_float_default(arg_value(argc, argv, "--min-db"), config->min_db);
    config->max_db = parse_float_default(arg_value(argc, argv, "--max-db"), config->max_db);
    config->auto_level = !arg_present(argc, argv, "--no-auto-level");
    config->log_iq_stats = arg_present(argc, argv, "--log-iq-stats");
    config->headless = arg_present(argc, argv, "--headless");
    config->control_url = arg_value(argc, argv, "--control-url");
    config->channel_id = (uint32_t)parse_u64_default(arg_value(argc, argv, "--channel"), 0);
    /* With --control-url the port and sample rate come from the REST API. */
    if (config->control_url == 0 && (config->port == 0 || config->sample_rate_hz == 0)) {
        return false;
    }
    if (!waterfall_fft_size_valid(config->fft_size) || config->rows == 0) {
        return false;
    }
    if (config->width <= 0 || config->height <= 0) {
        return false;
    }
    if (!config->auto_level && config->max_db <= config->min_db) {
        return false;
    }
    config->frame_stride_samples = waterfall_stride_for_history(config->rows, config->sample_rate_hz, config->history_seconds, config->fft_size);
    config->history_seconds = waterfall_history_for_stride(config->rows, config->sample_rate_hz, config->frame_stride_samples);
    return true;
}

static int open_udp_socket(const app_config_t *config)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    int rcvbuf = REQUESTED_RCVBUF_BYTES;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#ifdef SO_RCVBUFFORCE
    rcvbuf = REQUESTED_RCVBUF_BYTES;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf));
#endif

    struct in_addr requested_addr;
    if (inet_pton(AF_INET, config->host, &requested_addr) != 1) {
        close(fd);
        return -1;
    }
    struct in_addr interface_addr;
    if (inet_pton(AF_INET, config->interface_host, &interface_addr) != 1) {
        close(fd);
        return -1;
    }
    const bool multicast = ipv4_is_multicast(requested_addr);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->port);
    addr.sin_addr.s_addr = multicast ? htonl(INADDR_ANY) : requested_addr.s_addr;
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (multicast) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = requested_addr;
        mreq.imr_interface = interface_addr;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            close(fd);
            return -1;
        }
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static int socket_receive_buffer_bytes(int fd)
{
    int value = 0;
    socklen_t value_size = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &value_size) != 0) {
        return 0;
    }
    return value;
}

static void print_receive_buffer_warning(int rcvbuf_bytes)
{
    if (rcvbuf_bytes >= MIN_RECOMMENDED_RCVBUF_BYTES) {
        return;
    }
    fprintf(stderr,
            "warning: kernel receive buffer is only %d bytes; for 98 MS/s UDP use e.g. "
            "`sudo sysctl -w net.core.rmem_max=134217728 net.core.rmem_default=134217728`\n",
            rcvbuf_bytes);
}

static double elapsed_seconds(struct timespec start, struct timespec stop)
{
    return (double)(stop.tv_sec - start.tv_sec) + (double)(stop.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void rate_window_reset(rx_stats_t *stats)
{
    if (stats != 0) {
        stats->rate_window_samples = 0;
        stats->rate_window_start_ns = 0;
    }
}

static void update_sequence_stats(rx_stats_t *stats, const vita49_rx_packet_t *packet)
{
    if (stats->have_sequence) {
        const uint8_t expected = (uint8_t)((stats->last_sequence + 1U) & 0x0fU);
        if (packet->sequence != expected) {
            stats->sequence_gaps++;
        }
    }
    stats->have_sequence = true;
    stats->last_sequence = packet->sequence;
    stats->last_stream_id = packet->stream_id;
    stats->last_timestamp_ns = packet->timestamp_ns;
}

static void frame_sampler_reset(frame_sampler_t *sampler)
{
    sampler->collected_samples = 0U;
    sampler->samples_until_frame = 0U;
}

static size_t maybe_push_frames(
    waterfall_t *wf,
    const vita49_rx_packet_t *packet,
    frame_sampler_t *sampler,
    size_t frame_stride_samples)
{
    const size_t payload_samples = packet->payload_bytes / 4U;
    const int16_t *iq = (const int16_t *)(const void *)packet->payload;
    size_t pushed = 0U;
    size_t pos = 0U;

    while (pos < payload_samples) {
        if (sampler->collected_samples > 0U || sampler->samples_until_frame == 0U) {
            const size_t need = wf->fft_size - sampler->collected_samples;
            const size_t available = payload_samples - pos;
            const size_t take = need < available ? need : available;
            memcpy(sampler->iq + 2U * sampler->collected_samples, iq + 2U * pos, take * 2U * sizeof(*sampler->iq));
            sampler->collected_samples += take;
            pos += take;

            if (sampler->collected_samples < wf->fft_size) {
                break;
            }
            if (waterfall_push_ci16(wf, sampler->iq, wf->fft_size)) {
                pushed++;
            }
            sampler->collected_samples = 0U;
            sampler->samples_until_frame = frame_stride_samples > wf->fft_size ? frame_stride_samples - wf->fft_size : 0U;
            continue;
        }

        const size_t remaining = payload_samples - pos;
        if (sampler->samples_until_frame >= remaining) {
            sampler->samples_until_frame -= remaining;
            break;
        }
        pos += sampler->samples_until_frame;
        sampler->samples_until_frame = 0U;
    }

    return pushed;
}

static void print_status(const rx_stats_t *stats, const waterfall_t *wf, double seconds)
{
    const double msps = seconds > 0.0 ? (double)stats->payload_samples / seconds / 1000000.0 : 0.0;
    fprintf(stderr,
            "packets=%llu frames=%llu samples=%.3fM rate=%.2f MS/s gaps=%llu bad=%llu stream=0x%08x time_ns=%llu row=%.1f..%.1f dB\n",
            (unsigned long long)stats->packets,
            (unsigned long long)stats->frames,
            (double)stats->payload_samples / 1000000.0,
            msps,
            (unsigned long long)stats->sequence_gaps,
            (unsigned long long)stats->bad_packets,
            stats->last_stream_id,
            (unsigned long long)stats->last_timestamp_ns,
            (double)wf->last_min_db,
            (double)wf->last_max_db);
}

static void set_history_seconds(app_config_t *config, double seconds, frame_sampler_t *sampler)
{
    const double clamped = waterfall_clamp_history_seconds(seconds);
    const size_t stride = waterfall_stride_for_history(config->rows, config->sample_rate_hz, clamped, config->fft_size);
    if (stride == 0U) {
        return;
    }
    config->frame_stride_samples = stride;
    config->history_seconds = waterfall_history_for_stride(config->rows, config->sample_rate_hz, stride);
    frame_sampler_reset(sampler);
    fprintf(stderr, "waterfall history %.1fs stride=%zu samples\n", config->history_seconds, config->frame_stride_samples);
}

/* Apply a new stream sample rate: recompute the frame stride for the current history
 * target, restart frame collection, and clear the waterfall history (old rows cover a
 * different frequency span, so keeping them would be misleading). */
static void apply_sample_rate(app_config_t *config, waterfall_t *wf, frame_sampler_t *sampler, rx_stats_t *stats, uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U || sample_rate_hz == config->sample_rate_hz) {
        return;
    }
    rate_window_reset(stats);
    config->sample_rate_hz = sample_rate_hz;
    config->frame_stride_samples = waterfall_stride_for_history(config->rows, config->sample_rate_hz, config->history_seconds, config->fft_size);
    config->history_seconds = waterfall_history_for_stride(config->rows, config->sample_rate_hz, config->frame_stride_samples);
    if (stats != 0) {
        stats->discard_until_ns = monotonic_ns() + 150000000ULL;
    }
    if (sampler != 0) {
        frame_sampler_reset(sampler);
    }
    if (wf != 0) {
        waterfall_t next;
        if (waterfall_init(&next, wf->fft_size, wf->rows)) {
            waterfall_free(wf);
            *wf = next;
        }
    }
    fprintf(stderr,
            "stream sample rate %.3f MS/s history=%.1fs stride=%zu samples\n",
            (double)config->sample_rate_hz / 1000000.0,
            config->history_seconds,
            config->frame_stride_samples);
}

static const control_channel_t *control_current(const control_state_t *control)
{
    return &control->client.channels[control->channel];
}

/* Select a channel: re-read the channel list, enable its stream if needed, retarget the
 * UDP socket at its port, and adopt its sample rate. fd/wf/sampler/stats may be NULL for
 * the initial pre-socket selection. */
static bool control_select_channel(
    app_config_t *config,
    control_state_t *control,
    int *fd,
    waterfall_t *wf,
    frame_sampler_t *sampler,
    rx_stats_t *stats,
    uint32_t channel_id)
{
    char error[CONTROL_MAX_ERROR];
    if (!control_client_refresh(&control->client, error, sizeof(error))) {
        fprintf(stderr, "control: %s\n", error);
        return false;
    }
    if (channel_id >= control->client.channel_count) {
        fprintf(stderr, "control: channel %u does not exist (0..%zu)\n", channel_id, control->client.channel_count - 1U);
        return false;
    }
    control->channel = channel_id;
    const control_channel_t *channel = control_current(control);
    if (!channel->stream_enabled &&
        !control_client_set_stream(&control->client, channel_id, true, error, sizeof(error))) {
        fprintf(stderr, "control: %s\n", error);
    }
    config->port = channel->udp_port;
    apply_sample_rate(config, wf, sampler, stats, channel->sample_rate_hz);
    if (stats != 0) {
        stats->stream_center_hz = channel->center_frequency_hz;
        stats->stream_bandwidth_hz = channel->bandwidth_hz;
        stats->have_sequence = false;
        rate_window_reset(stats);
    }
    if (fd != 0 && *fd >= 0) {
        close(*fd);
        *fd = open_udp_socket(config);
        if (*fd < 0) {
            fprintf(stderr, "failed to bind UDP %s:%u: %s\n", config->host, config->port, strerror(errno));
            return false;
        }
    }
    fprintf(stderr,
            "channel %u: port=%u center=%.6f MHz bw=%.3f MHz rate=%.3f MS/s%s%s\n",
            channel->channel_id,
            channel->udp_port,
            (double)channel->center_frequency_hz / 1000000.0,
            (double)channel->bandwidth_hz / 1000000.0,
            (double)channel->sample_rate_hz / 1000000.0,
            channel->track_tuner ? " (tracks tuner)" : "",
            channel->in_frontend_window ? "" : " (outside front-end window)");
    return true;
}

static uint64_t control_frequency_step_hz(const control_channel_t *channel)
{
    const uint64_t step = channel->bandwidth_hz / 10U;
    return step < 100000ULL ? 100000ULL : step;
}

/* Set the selected channel's center frequency (clamped to the receiver's limits). */
static void control_set_center(control_state_t *control, rx_stats_t *stats, uint64_t center_hz)
{
    const control_channel_t *channel = control_current(control);
    if (channel->track_tuner) {
        fprintf(stderr, "control: channel %u follows the receiver tuner; it cannot be retuned directly\n", channel->channel_id);
        return;
    }
    if (control->client.frequency_max_hz > 0U && center_hz > control->client.frequency_max_hz) {
        center_hz = control->client.frequency_max_hz;
    }
    char error[CONTROL_MAX_ERROR];
    if (!control_client_set_channel(&control->client, control->channel, &center_hz, 0, error, sizeof(error))) {
        fprintf(stderr, "control: %s\n", error);
        return;
    }
    const control_channel_t *updated = control_current(control);
    if (stats != 0) {
        stats->stream_center_hz = updated->center_frequency_hz;
    }
    fprintf(stderr,
            "channel %u center %.6f MHz%s\n",
            updated->channel_id,
            (double)updated->center_frequency_hz / 1000000.0,
            updated->in_frontend_window ? "" : " (outside front-end window: stream is empty)");
}

static void control_step_frequency(control_state_t *control, rx_stats_t *stats, int direction, bool big_step)
{
    const control_channel_t *channel = control_current(control);
    if (channel->track_tuner) {
        fprintf(stderr, "control: channel %u follows the receiver tuner; it cannot be retuned directly\n", channel->channel_id);
        return;
    }
    const uint64_t step = control_frequency_step_hz(channel) * (big_step ? 10U : 1U);
    uint64_t center = channel->center_frequency_hz;
    if (direction < 0) {
        center = center > step ? center - step : 0U;
    } else {
        center += step;
    }
    control_set_center(control, stats, center);
}

/* Fixed-mode receiver center: the midpoint of the tuner span (matches the simulator's
 * receiver_fixed_center_hz, including its round-half-up on two odd endpoints). */
static uint64_t control_receiver_center_hz(const control_state_t *control)
{
    const control_client_t *client = &control->client;
    return client->tuner_start_hz / 2U + client->tuner_stop_hz / 2U +
        ((client->tuner_start_hz & 1U) && (client->tuner_stop_hz & 1U) ? 1U : 0U);
}

/* Move the receiver tuner to a new center, preserving the current span (clamped to the
 * receiver's frequency limits). Tuner-tracking channels follow automatically, so the channel
 * list is re-read afterwards to update their reported center and in-window flags. */
static void control_set_receiver_center(control_state_t *control, rx_stats_t *stats, uint64_t center_hz)
{
    control_client_t *client = &control->client;
    const uint64_t span = client->tuner_stop_hz > client->tuner_start_hz
        ? client->tuner_stop_hz - client->tuner_start_hz
        : 0U;
    const uint64_t low_half = span / 2U;
    const uint64_t high_half = span - low_half;
    if (center_hz < client->frequency_min_hz + low_half) {
        center_hz = client->frequency_min_hz + low_half;
    }
    if (client->frequency_max_hz > 0U && center_hz + high_half > client->frequency_max_hz) {
        center_hz = client->frequency_max_hz - high_half;
    }
    const uint64_t start = center_hz - low_half;
    const uint64_t stop = center_hz + high_half;

    char error[CONTROL_MAX_ERROR];
    if (!control_client_set_frequency_range(client, start, stop, error, sizeof(error))) {
        fprintf(stderr, "control: %s\n", error);
        return;
    }
    /* Channel 0 (and any other tuner-tracking channel) just moved with the tuner; refresh so
     * the frequency field and out-of-window flags reflect it. Best-effort: a failed refresh
     * still leaves the tuner moved. */
    if (!control_client_refresh(client, error, sizeof(error))) {
        fprintf(stderr, "control: %s\n", error);
    }
    if (stats != 0 && control_current(control)->track_tuner) {
        stats->stream_center_hz = control_current(control)->center_frequency_hz;
    }
    fprintf(stderr, "receiver center %.6f MHz (span %.3f MHz)\n",
            (double)control_receiver_center_hz(control) / 1000000.0,
            (double)span / 1000000.0);
}

/* Switch the channel to a supported bandwidth profile. The paired sample rate is
 * applied by the server; the waterfall follows it. */
static void control_set_bandwidth(app_config_t *config, control_state_t *control, waterfall_t *wf, frame_sampler_t *sampler, rx_stats_t *stats, uint32_t bandwidth_hz)
{
    const control_channel_t *channel = control_current(control);
    if (bandwidth_hz == channel->bandwidth_hz) {
        return;
    }
    char error[CONTROL_MAX_ERROR];
    if (!control_client_set_channel(&control->client, control->channel, 0, &bandwidth_hz, error, sizeof(error))) {
        fprintf(stderr, "control: %s\n", error);
        return;
    }
    const control_channel_t *updated = control_current(control);
    if (stats != 0) {
        stats->stream_bandwidth_hz = updated->bandwidth_hz;
    }
    apply_sample_rate(config, wf, sampler, stats, updated->sample_rate_hz);
    fprintf(stderr,
            "channel %u bandwidth %.3f MHz rate %.3f MS/s%s\n",
            updated->channel_id,
            (double)updated->bandwidth_hz / 1000000.0,
            (double)updated->sample_rate_hz / 1000000.0,
            updated->in_frontend_window ? "" : " (outside front-end window: stream is empty)");
}

/* Bandwidth profiles that fit the front end, in capability order. */
static size_t control_bandwidth_options(const control_state_t *control, uint32_t *out, size_t max)
{
    const control_client_t *client = &control->client;
    size_t count = 0;
    for (size_t i = 0; i < client->profile_count && count < max; i++) {
        if ((uint64_t)client->profiles[i].bandwidth_hz <= client->frontend_bandwidth_hz) {
            out[count++] = client->profiles[i].bandwidth_hz;
        }
    }
    return count;
}

/* Cycle to the next supported bandwidth profile that fits the front end (keyboard `B`). */
static void control_cycle_bandwidth(app_config_t *config, control_state_t *control, waterfall_t *wf, frame_sampler_t *sampler, rx_stats_t *stats)
{
    uint32_t options[CONTROL_MAX_PROFILES];
    const size_t count = control_bandwidth_options(control, options, CONTROL_MAX_PROFILES);
    if (count < 2U) {
        return;
    }
    const control_channel_t *channel = control_current(control);
    size_t current = 0;
    for (size_t i = 0; i < count; i++) {
        if (options[i] == channel->bandwidth_hz) {
            current = i;
            break;
        }
    }
    control_set_bandwidth(config, control, wf, sampler, stats, options[(current + 1U) % count]);
}

static void maybe_log_iq_stats(const app_config_t *config, const rx_stats_t *stats, const vita49_rx_packet_t *packet)
{
    if (!config->log_iq_stats || packet->payload_bytes < 4U) {
        return;
    }
    if (stats->packets > 8U && (stats->packets % 1000U) != 0U) {
        return;
    }

    const size_t component_count = packet->payload_bytes / 2U;
    const int16_t *components = (const int16_t *)(const void *)packet->payload;
    int16_t min_value = components[0];
    int16_t max_value = components[0];
    size_t nonzero = 0;
    for (size_t i = 0; i < component_count; i++) {
        if (components[i] < min_value) {
            min_value = components[i];
        }
        if (components[i] > max_value) {
            max_value = components[i];
        }
        if (components[i] != 0) {
            nonzero++;
        }
    }
    fprintf(stderr,
            "iq packet=%llu stream=0x%08x samples=%zu i16_min=%d i16_max=%d nonzero=%zu/%zu\n",
            (unsigned long long)stats->packets,
            packet->stream_id,
            packet->payload_bytes / 4U,
            (int)min_value,
            (int)max_value,
            nonzero,
            component_count);
}

static bool handle_received_packet(
    app_config_t *config,
    waterfall_t *waterfall,
    rx_stats_t *stats,
    frame_sampler_t *sampler,
    const uint8_t *packet_data,
    size_t packet_bytes,
    bool *frame_ready)
{
    if (vita49_rx_packet_type(packet_data, packet_bytes) == VITA49_RX_PACKET_TYPE_CONTEXT) {
        vita49_rx_context_t context;
        if (!vita49_rx_parse_context(packet_data, packet_bytes, &context)) {
            stats->bad_packets++;
            return false;
        }
        /* In-band stream configuration: follow retunes and rate changes made by any
         * client without polling the REST API. */
        stats->context_packets++;
        stats->bytes += (uint64_t)packet_bytes;
        stats->stream_center_hz = context.rf_reference_frequency_hz;
        stats->stream_bandwidth_hz = context.bandwidth_hz;
        apply_sample_rate(config, waterfall, sampler, stats, (uint32_t)context.sample_rate_hz);
        return false;
    }
    vita49_rx_packet_t packet;
    if (!vita49_rx_parse_if_data(packet_data, packet_bytes, &packet)) {
        stats->bad_packets++;
        return false;
    }
    if (stats->discard_until_ns != 0U) {
        if (monotonic_ns() < stats->discard_until_ns) {
            return false; /* stale-rate payloads from before the reconfiguration */
        }
        stats->discard_until_ns = 0U;
        stats->have_sequence = false;
        frame_sampler_reset(sampler);
    }
    update_sequence_stats(stats, &packet);
    stats->packets++;
    stats->bytes += (uint64_t)packet_bytes;
    stats->payload_samples += (uint64_t)(packet.payload_bytes / 4U);
    if (stats->rate_window_start_ns == 0U) {
        stats->rate_window_start_ns = monotonic_ns();
    }
    stats->rate_window_samples += (uint64_t)(packet.payload_bytes / 4U);
    maybe_log_iq_stats(config, stats, &packet);

    const size_t frames = maybe_push_frames(waterfall, &packet, sampler, config->frame_stride_samples);
    if (frames > 0U) {
        stats->frames += (uint64_t)frames;
        *frame_ready = true;
    }
    return (config->max_packets > 0 && stats->packets >= config->max_packets) ||
           (config->max_frames > 0 && stats->frames >= config->max_frames);
}

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    size_t pixel_count;
    bool level_initialized;
    float display_min_db;
    float display_max_db;
    SDL_Rect history_minus_button;
    SDL_Rect history_plus_button;
    /* Control widgets (drawn and hit-tested only with --control-url). */
    SDL_Rect rx_freq_field;
    SDL_Rect channel_combo;
    SDL_Rect freq_field;
    SDL_Rect bandwidth_combo;
    int open_combo; /* 0 = none, 1 = channel, 2 = bandwidth */
    bool freq_editing;
    char freq_edit[UI_FREQ_EDIT_MAX];
    bool rx_freq_editing;
    char rx_freq_edit[UI_FREQ_EDIT_MAX];
} ui_t;

#define UI_COMBO_NONE 0
#define UI_COMBO_CHANNEL 1
#define UI_COMBO_BANDWIDTH 2

#ifdef __linux__
typedef struct {
    uint8_t *storage;
    struct mmsghdr messages[RX_BATCH_SIZE];
    struct iovec iovecs[RX_BATCH_SIZE];
} rx_batch_t;
#endif

static uint32_t color_map(float db, float min_db, float max_db)
{
    float x = (db - min_db) / (max_db - min_db);
    if (x < 0.0f) {
        x = 0.0f;
    }
    if (x > 1.0f) {
        x = 1.0f;
    }
    const float r = x < 0.55f ? 0.0f : (x - 0.55f) / 0.45f;
    const float g = x < 0.25f ? 0.0f : (x < 0.75f ? (x - 0.25f) / 0.5f : 1.0f);
    const float b = x < 0.5f ? 0.25f + x : 1.0f - (x - 0.5f) * 2.0f;
    const uint8_t rr = (uint8_t)(255.0f * r);
    const uint8_t gg = (uint8_t)(255.0f * g);
    const uint8_t bb = (uint8_t)(255.0f * (b < 0.0f ? 0.0f : b));
    return 0xff000000U | ((uint32_t)rr << 16U) | ((uint32_t)gg << 8U) | (uint32_t)bb;
}

static float approach_float(float current, float target)
{
    const float alpha = target > current ? AUTO_LEVEL_ATTACK : AUTO_LEVEL_RELEASE;
    return current + alpha * (target - current);
}

static float waterfall_visible_peak_db(const waterfall_t *wf)
{
    float peak = WATERFALL_EMPTY_DB;
    const size_t count = wf->rows * wf->fft_size;
    for (size_t i = 0; i < count; i++) {
        if (wf->history[i] > peak) {
            peak = wf->history[i];
        }
    }
    return peak;
}

static void compute_display_range(ui_t *ui, const waterfall_t *wf, const app_config_t *config, float *min_db, float *max_db)
{
    if (!config->auto_level) {
        ui->level_initialized = false;
        *min_db = config->min_db;
        *max_db = config->max_db;
        return;
    }

    const float peak = waterfall_visible_peak_db(wf);
    float target_hi = config->max_db;
    if (peak > WATERFALL_EMPTY_DB) {
        target_hi = fmaxf(config->max_db, peak + AUTO_LEVEL_HEADROOM_DB);
    }
    float target_lo = target_hi - AUTO_LEVEL_DYNAMIC_RANGE_DB;
    target_lo = fmaxf(config->min_db, target_lo);

    if (!ui->level_initialized) {
        ui->display_min_db = target_lo;
        ui->display_max_db = target_hi;
        ui->level_initialized = true;
    } else {
        ui->display_min_db = approach_float(ui->display_min_db, target_lo);
        ui->display_max_db = approach_float(ui->display_max_db, target_hi);
    }
    if (ui->display_max_db <= ui->display_min_db + 1.0f) {
        ui->display_max_db = ui->display_min_db + 1.0f;
    }

    *min_db = ui->display_min_db;
    *max_db = ui->display_max_db;
}

#ifdef __linux__
static bool rx_batch_init(rx_batch_t *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->storage = malloc(RX_BATCH_SIZE * MAX_PACKET_BYTES);
    if (batch->storage == 0) {
        return false;
    }
    for (size_t i = 0; i < RX_BATCH_SIZE; i++) {
        batch->iovecs[i].iov_base = batch->storage + i * MAX_PACKET_BYTES;
        batch->iovecs[i].iov_len = MAX_PACKET_BYTES;
        batch->messages[i].msg_hdr.msg_iov = &batch->iovecs[i];
        batch->messages[i].msg_hdr.msg_iovlen = 1;
    }
    return true;
}

static void rx_batch_free(rx_batch_t *batch)
{
    if (batch != 0) {
        free(batch->storage);
        memset(batch, 0, sizeof(*batch));
    }
}
#endif

static bool ui_recreate_texture(ui_t *ui, const app_config_t *config)
{
    SDL_Texture *texture = SDL_CreateTexture(ui->renderer,
                                             SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             (int)config->fft_size,
                                             (int)config->rows);
    const size_t pixel_count = config->fft_size * config->rows;
    uint32_t *pixels = calloc(pixel_count, sizeof(*pixels));
    if (texture == 0 || pixels == 0) {
        fprintf(stderr, "SDL texture allocation failed: %s\n", SDL_GetError());
        SDL_DestroyTexture(texture);
        free(pixels);
        return false;
    }
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    SDL_DestroyTexture(ui->texture);
    free(ui->pixels);
    ui->texture = texture;
    ui->pixels = pixels;
    ui->pixel_count = pixel_count;
    ui->level_initialized = false;
    return true;
}

static bool ui_init(ui_t *ui, const app_config_t *config)
{
    memset(ui, 0, sizeof(*ui));
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    ui->window = SDL_CreateWindow("SDR VITA 49.2 Waterfall",
                                  SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED,
                                  config->width,
                                  config->height,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (ui->window == 0) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    ui->renderer = SDL_CreateRenderer(ui->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (ui->renderer == 0) {
        ui->renderer = SDL_CreateRenderer(ui->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (ui->renderer == 0) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(ui->window);
        SDL_Quit();
        return false;
    }
    if (!ui_recreate_texture(ui, config)) {
        SDL_DestroyRenderer(ui->renderer);
        SDL_DestroyWindow(ui->window);
        SDL_Quit();
        return false;
    }
    return true;
}

static void ui_free(ui_t *ui)
{
    if (ui == 0) {
        return;
    }
    free(ui->pixels);
    SDL_DestroyTexture(ui->texture);
    SDL_DestroyRenderer(ui->renderer);
    SDL_DestroyWindow(ui->window);
    SDL_Quit();
}

static bool set_waterfall_rows(
    waterfall_t *wf,
    ui_t *ui,
    app_config_t *config,
    size_t rows,
    frame_sampler_t *sampler)
{
    if (rows == 0U || rows == config->rows) {
        return true;
    }

    waterfall_t next;
    if (!waterfall_init(&next, config->fft_size, rows)) {
        fprintf(stderr, "failed to resize waterfall to %zu rows\n", rows);
        return false;
    }

    const size_t old_rows = config->rows;
    const size_t old_stride = config->frame_stride_samples;
    const double old_history = config->history_seconds;
    config->rows = rows;
    config->frame_stride_samples = waterfall_stride_for_history(config->rows, config->sample_rate_hz, config->history_seconds, config->fft_size);
    config->history_seconds = waterfall_history_for_stride(config->rows, config->sample_rate_hz, config->frame_stride_samples);

    if (!ui_recreate_texture(ui, config)) {
        config->rows = old_rows;
        config->frame_stride_samples = old_stride;
        config->history_seconds = old_history;
        waterfall_free(&next);
        return false;
    }

    waterfall_free(wf);
    *wf = next;
    frame_sampler_reset(sampler);
    fprintf(stderr,
            "waterfall rows=%zu history=%.1fs stride=%zu samples\n",
            config->rows,
            config->history_seconds,
            config->frame_stride_samples);
    return true;
}

static void ui_layout(ui_t *ui, int window_width)
{
    const int button_y = (UI_TOOLBAR_HEIGHT - UI_BUTTON_SIZE) / 2;
    ui->history_plus_button = (SDL_Rect){
        .x = window_width - UI_BUTTON_MARGIN - UI_BUTTON_SIZE,
        .y = button_y,
        .w = UI_BUTTON_SIZE,
        .h = UI_BUTTON_SIZE,
    };
    ui->history_minus_button = (SDL_Rect){
        .x = ui->history_plus_button.x - UI_BUTTON_MARGIN - UI_BUTTON_SIZE,
        .y = button_y,
        .w = UI_BUTTON_SIZE,
        .h = UI_BUTTON_SIZE,
    };

    /* Control group on the left: receiver-tuner field ("RX"), channel combo, channel
     * frequency input field + MHz label, bandwidth combo. */
    int x = UI_BUTTON_MARGIN;
    x += ui_text_width(UI_TEXT_SCALE, "RX") + 4;
    ui->rx_freq_field = (SDL_Rect){.x = x, .y = button_y, .w = UI_FREQ_FIELD_WIDTH, .h = UI_BUTTON_SIZE};
    x += UI_FREQ_FIELD_WIDTH + 4 + ui_text_width(UI_TEXT_SCALE, "MHZ") + 2 * UI_BUTTON_MARGIN;
    ui->channel_combo = (SDL_Rect){.x = x, .y = button_y, .w = UI_CHANNEL_COMBO_WIDTH, .h = UI_BUTTON_SIZE};
    x += UI_CHANNEL_COMBO_WIDTH + 2 * UI_BUTTON_MARGIN;
    ui->freq_field = (SDL_Rect){.x = x, .y = button_y, .w = UI_FREQ_FIELD_WIDTH, .h = UI_BUTTON_SIZE};
    x += UI_FREQ_FIELD_WIDTH + 4 + ui_text_width(UI_TEXT_SCALE, "MHZ") + 2 * UI_BUTTON_MARGIN;
    ui->bandwidth_combo = (SDL_Rect){.x = x, .y = button_y, .w = UI_BANDWIDTH_COMBO_WIDTH, .h = UI_BUTTON_SIZE};
}

static bool ui_point_in_rect(int x, int y, const SDL_Rect *rect)
{
    return x >= rect->x && x < rect->x + rect->w && y >= rect->y && y < rect->y + rect->h;
}

static void ui_draw_button(SDL_Renderer *renderer, SDL_Rect rect, bool enabled, bool plus)
{
    if (enabled) {
        SDL_SetRenderDrawColor(renderer, 48, 55, 64, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 28, 32, 38, 255);
    }
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, 104, 116, 132, 255);
    SDL_RenderDrawRect(renderer, &rect);

    const int center_x = rect.x + rect.w / 2;
    const int center_y = rect.y + rect.h / 2;
    const int arm = rect.w / 5;
    if (enabled) {
        SDL_SetRenderDrawColor(renderer, 230, 236, 244, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 110, 118, 128, 255);
    }
    SDL_RenderDrawLine(renderer, center_x - arm, center_y, center_x + arm, center_y);
    if (plus) {
        SDL_RenderDrawLine(renderer, center_x, center_y - arm, center_x, center_y + arm);
    }
}

static void ui_draw_button_frame(SDL_Renderer *renderer, SDL_Rect rect, bool enabled)
{
    if (enabled) {
        SDL_SetRenderDrawColor(renderer, 48, 55, 64, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 28, 32, 38, 255);
    }
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, 104, 116, 132, 255);
    SDL_RenderDrawRect(renderer, &rect);
    if (enabled) {
        SDL_SetRenderDrawColor(renderer, 230, 236, 244, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 110, 118, 128, 255);
    }
}

/* Vertically centered widget text baseline. */
static int ui_widget_text_y(SDL_Rect rect)
{
    return rect.y + (rect.h - ui_text_height(UI_TEXT_SCALE)) / 2;
}

/* Combo box: label plus a small down-triangle on the right. */
static void ui_draw_combo(SDL_Renderer *renderer, SDL_Rect rect, const char *label, bool open)
{
    ui_draw_button_frame(renderer, rect, true);
    if (open) {
        SDL_SetRenderDrawColor(renderer, 79, 190, 160, 255);
        SDL_RenderDrawRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 230, 236, 244, 255);
    }
    ui_text_draw(renderer, rect.x + 6, ui_widget_text_y(rect), UI_TEXT_SCALE, label);
    const int arrow_x = rect.x + rect.w - 14;
    const int arrow_y = rect.y + rect.h / 2 - 2;
    for (int row = 0; row < 4; row++) {
        SDL_RenderDrawLine(renderer, arrow_x - (3 - row), arrow_y + row, arrow_x + (3 - row), arrow_y + row);
    }
}

/* Frequency input field. While editing it shows the typed text with a caret and a
 * highlighted border; disabled (tuner-tracking channel) renders greyed out. */
static void ui_draw_freq_field(SDL_Renderer *renderer, SDL_Rect rect, const char *text, bool editing, bool enabled)
{
    SDL_SetRenderDrawColor(renderer, editing ? 10 : 24, editing ? 14 : 28, editing ? 18 : 34, 255);
    SDL_RenderFillRect(renderer, &rect);
    if (editing) {
        SDL_SetRenderDrawColor(renderer, 79, 190, 160, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 104, 116, 132, 255);
    }
    SDL_RenderDrawRect(renderer, &rect);
    if (enabled) {
        SDL_SetRenderDrawColor(renderer, 230, 236, 244, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 110, 118, 128, 255);
    }
    char shown[UI_FREQ_EDIT_MAX + 2];
    snprintf(shown, sizeof(shown), "%s%s", text, editing ? "_" : "");
    ui_text_draw(renderer, rect.x + 6, ui_widget_text_y(rect), UI_TEXT_SCALE, shown);
}

static SDL_Rect ui_dropdown_item_rect(int x, size_t index)
{
    return (SDL_Rect){
        .x = x,
        .y = UI_TOOLBAR_HEIGHT + (int)index * UI_DROPDOWN_ITEM_HEIGHT,
        .w = UI_DROPDOWN_WIDTH,
        .h = UI_DROPDOWN_ITEM_HEIGHT,
    };
}

static void ui_channel_item_label(const control_channel_t *channel, char *out, size_t out_size)
{
    if (channel->track_tuner) {
        snprintf(out, out_size, "CH%u TUNER %.0fM", channel->channel_id, (double)channel->bandwidth_hz / 1000000.0);
    } else {
        snprintf(out,
                 out_size,
                 "CH%u %.3f MHZ %.0fM",
                 channel->channel_id,
                 (double)channel->center_frequency_hz / 1000000.0,
                 (double)channel->bandwidth_hz / 1000000.0);
    }
}

static void ui_bandwidth_item_label(const control_state_t *control, uint32_t bandwidth_hz, char *out, size_t out_size)
{
    const control_client_t *client = &control->client;
    for (size_t i = 0; i < client->profile_count; i++) {
        if (client->profiles[i].bandwidth_hz == bandwidth_hz) {
            snprintf(out,
                     out_size,
                     "%.3f MHZ / %.3f MSPS",
                     (double)bandwidth_hz / 1000000.0,
                     (double)client->profiles[i].sample_rate_hz / 1000000.0);
            return;
        }
    }
    snprintf(out, out_size, "%.3f MHZ", (double)bandwidth_hz / 1000000.0);
}

static void ui_draw_dropdown_item(SDL_Renderer *renderer, SDL_Rect rect, const char *label, bool selected)
{
    if (selected) {
        SDL_SetRenderDrawColor(renderer, 34, 78, 66, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 24, 30, 37, 255);
    }
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, 104, 116, 132, 255);
    SDL_RenderDrawRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, 230, 236, 244, 255);
    ui_text_draw(renderer, rect.x + 6, ui_widget_text_y(rect), UI_TEXT_SCALE, label);
}

/* Open dropdown lists, drawn last so they overlay the spectrum. */
static void ui_draw_dropdowns(ui_t *ui, const control_state_t *control)
{
    if (!control->enabled || ui->open_combo == UI_COMBO_NONE) {
        return;
    }
    if (ui->open_combo == UI_COMBO_CHANNEL) {
        for (size_t i = 0; i < control->client.channel_count; i++) {
            char label[64];
            ui_channel_item_label(&control->client.channels[i], label, sizeof(label));
            ui_draw_dropdown_item(ui->renderer, ui_dropdown_item_rect(ui->channel_combo.x, i), label, i == control->channel);
        }
    } else if (ui->open_combo == UI_COMBO_BANDWIDTH) {
        uint32_t options[CONTROL_MAX_PROFILES];
        const size_t count = control_bandwidth_options(control, options, CONTROL_MAX_PROFILES);
        const control_channel_t *channel = control_current(control);
        for (size_t i = 0; i < count; i++) {
            char label[64];
            ui_bandwidth_item_label(control, options[i], label, sizeof(label));
            ui_draw_dropdown_item(ui->renderer, ui_dropdown_item_rect(ui->bandwidth_combo.x, i), label, options[i] == channel->bandwidth_hz);
        }
    }
}

/* Frequency-field edit lifecycle. SDL text input is only active while editing, so the
 * single-key shortcuts (0-7, B) stay usable otherwise. */
static void freq_edit_begin(ui_t *ui, const control_state_t *control, const rx_stats_t *stats)
{
    const control_channel_t *channel = control_current(control);
    if (channel->track_tuner) {
        fprintf(stderr, "control: channel %u follows the receiver tuner; it cannot be retuned directly\n", channel->channel_id);
        return;
    }
    const uint64_t center = stats->stream_center_hz > 0U ? stats->stream_center_hz : channel->center_frequency_hz;
    snprintf(ui->freq_edit, sizeof(ui->freq_edit), "%.6f", (double)center / 1000000.0);
    ui->freq_editing = true;
    ui->open_combo = UI_COMBO_NONE;
    SDL_StartTextInput();
}

static void freq_edit_cancel(ui_t *ui)
{
    if (ui->freq_editing) {
        ui->freq_editing = false;
        SDL_StopTextInput();
    }
}

static void freq_edit_commit(ui_t *ui, control_state_t *control, rx_stats_t *stats)
{
    const double mhz = strtod(ui->freq_edit, 0);
    freq_edit_cancel(ui);
    if (!(mhz > 0.0)) {
        fprintf(stderr, "control: invalid frequency '%s'\n", ui->freq_edit);
        return;
    }
    control_set_center(control, stats, (uint64_t)llround(mhz * 1000000.0));
}

/* Append the digits/decimal point of `text` to a frequency-edit buffer (shared by the channel
 * and receiver fields). */
static void freq_text_append(char *buffer, size_t buffer_size, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        if ((*p < '0' || *p > '9') && *p != '.') {
            continue;
        }
        const size_t len = strlen(buffer);
        if (len + 1U >= buffer_size) {
            return;
        }
        buffer[len] = *p;
        buffer[len + 1U] = '\0';
    }
}

static void freq_text_backspace(char *buffer)
{
    const size_t len = strlen(buffer);
    if (len > 0U) {
        buffer[len - 1U] = '\0';
    }
}

/* Receiver-tuner field edit lifecycle, mirroring the channel frequency field. The receiver can
 * always be moved (no tuner-tracking restriction), so this field is never disabled. */
static void rx_freq_edit_begin(ui_t *ui, const control_state_t *control)
{
    snprintf(ui->rx_freq_edit, sizeof(ui->rx_freq_edit), "%.6f", (double)control_receiver_center_hz(control) / 1000000.0);
    ui->rx_freq_editing = true;
    ui->freq_editing = false;
    ui->open_combo = UI_COMBO_NONE;
    SDL_StartTextInput();
}

static void rx_freq_edit_cancel(ui_t *ui)
{
    if (ui->rx_freq_editing) {
        ui->rx_freq_editing = false;
        SDL_StopTextInput();
    }
}

static void rx_freq_edit_commit(ui_t *ui, control_state_t *control, rx_stats_t *stats)
{
    const double mhz = strtod(ui->rx_freq_edit, 0);
    const bool valid = mhz > 0.0;
    rx_freq_edit_cancel(ui);
    if (!valid) {
        fprintf(stderr, "control: invalid receiver frequency '%s'\n", ui->rx_freq_edit);
        return;
    }
    control_set_receiver_center(control, stats, (uint64_t)llround(mhz * 1000000.0));
}

/* When the in-band VITA context reports a configuration that contradicts the cached REST
 * channel state, another client changed the channel: re-read the channel list so the combo
 * boxes, frequency field, and out-of-window flag follow. Throttled to avoid hammering the
 * API while it is unreachable. */
static bool control_sync_with_stream(control_state_t *control, const rx_stats_t *stats)
{
    if (!control->enabled) {
        return false;
    }
    const control_channel_t *channel = control_current(control);
    const bool bandwidth_mismatch = stats->stream_bandwidth_hz != 0U &&
        stats->stream_bandwidth_hz != (uint64_t)channel->bandwidth_hz;
    const bool center_mismatch = !channel->track_tuner && stats->stream_center_hz != 0U &&
        stats->stream_center_hz != channel->center_frequency_hz;
    if (!bandwidth_mismatch && !center_mismatch) {
        return false;
    }
    const uint64_t now_ns = monotonic_ns();
    if (now_ns - control->last_refresh_attempt_ns < 500000000ULL) {
        return false;
    }
    control->last_refresh_attempt_ns = now_ns;
    char error[CONTROL_MAX_ERROR];
    if (!control_client_refresh(&control->client, error, sizeof(error))) {
        return false;
    }
    return true;
}

static void ui_draw_toolbar(ui_t *ui, const app_config_t *config, const control_state_t *control, const rx_stats_t *stats, int window_width)
{
    ui_layout(ui, window_width);

    SDL_Rect toolbar = {.x = 0, .y = 0, .w = window_width, .h = UI_TOOLBAR_HEIGHT};
    SDL_SetRenderDrawColor(ui->renderer, 17, 22, 28, 255);
    SDL_RenderFillRect(ui->renderer, &toolbar);

    if (control->enabled) {
        const control_channel_t *channel = control_current(control);

        /* Receiver tuner field ("RX"): moves the whole front-end window; tuner-tracking
         * channels follow. Always editable. */
        SDL_SetRenderDrawColor(ui->renderer, 160, 172, 186, 255);
        ui_text_draw(ui->renderer, ui->rx_freq_field.x - ui_text_width(UI_TEXT_SCALE, "RX") - 4, ui_widget_text_y(ui->rx_freq_field), UI_TEXT_SCALE, "RX");
        char rx_text[UI_FREQ_EDIT_MAX];
        if (ui->rx_freq_editing) {
            snprintf(rx_text, sizeof(rx_text), "%s", ui->rx_freq_edit);
        } else {
            snprintf(rx_text, sizeof(rx_text), "%.6f", (double)control_receiver_center_hz(control) / 1000000.0);
        }
        ui_draw_freq_field(ui->renderer, ui->rx_freq_field, rx_text, ui->rx_freq_editing, true);
        SDL_SetRenderDrawColor(ui->renderer, 160, 172, 186, 255);
        ui_text_draw(ui->renderer, ui->rx_freq_field.x + ui->rx_freq_field.w + 6, ui_widget_text_y(ui->rx_freq_field), UI_TEXT_SCALE, "MHZ");

        char channel_label[16];
        snprintf(channel_label, sizeof(channel_label), "CH%u", control->channel);
        ui_draw_combo(ui->renderer, ui->channel_combo, channel_label, ui->open_combo == UI_COMBO_CHANNEL);

        char freq_text[UI_FREQ_EDIT_MAX];
        if (ui->freq_editing) {
            snprintf(freq_text, sizeof(freq_text), "%s", ui->freq_edit);
        } else {
            const uint64_t center = stats->stream_center_hz > 0U ? stats->stream_center_hz : channel->center_frequency_hz;
            snprintf(freq_text, sizeof(freq_text), "%.6f", (double)center / 1000000.0);
        }
        ui_draw_freq_field(ui->renderer, ui->freq_field, freq_text, ui->freq_editing, !channel->track_tuner);
        SDL_SetRenderDrawColor(ui->renderer, 160, 172, 186, 255);
        ui_text_draw(ui->renderer, ui->freq_field.x + ui->freq_field.w + 6, ui_widget_text_y(ui->freq_field), UI_TEXT_SCALE, "MHZ");

        char bandwidth_label[16];
        snprintf(bandwidth_label, sizeof(bandwidth_label), "%.0fM", (double)channel->bandwidth_hz / 1000000.0);
        ui_draw_combo(ui->renderer, ui->bandwidth_combo, bandwidth_label, ui->open_combo == UI_COMBO_BANDWIDTH);
    }

    int bar_x = control->enabled
        ? ui->bandwidth_combo.x + ui->bandwidth_combo.w + 2 * UI_BUTTON_MARGIN
        : UI_BUTTON_MARGIN;
    if (control->enabled && !control_current(control)->in_frontend_window) {
        /* The channel span does not fit the front-end window: the stream is intentionally
         * empty (like a hardware DDC tuned outside the digitised band). Say so. */
        SDL_SetRenderDrawColor(ui->renderer, 255, 160, 60, 255);
        ui_text_draw(ui->renderer, bar_x, ui_widget_text_y(ui->bandwidth_combo), UI_TEXT_SCALE, "OUT OF WINDOW");
        bar_x += ui_text_width(UI_TEXT_SCALE, "OUT OF WINDOW") + 2 * UI_BUTTON_MARGIN;
    }
    const int bar_y = UI_TOOLBAR_HEIGHT / 2 - 4;
    const int bar_w = ui->history_minus_button.x - bar_x - UI_BUTTON_MARGIN;
    if (bar_w > 20) {
        SDL_Rect bar = {.x = bar_x, .y = bar_y, .w = bar_w, .h = 8};
        SDL_SetRenderDrawColor(ui->renderer, 44, 52, 61, 255);
        SDL_RenderFillRect(ui->renderer, &bar);
        const double span = WATERFALL_HISTORY_MAX_SECONDS - WATERFALL_HISTORY_MIN_SECONDS;
        double ratio = (config->history_seconds - WATERFALL_HISTORY_MIN_SECONDS) / span;
        if (ratio < 0.0) {
            ratio = 0.0;
        } else if (ratio > 1.0) {
            ratio = 1.0;
        }
        SDL_Rect fill = bar;
        fill.w = (int)((double)bar.w * ratio);
        SDL_SetRenderDrawColor(ui->renderer, 79, 190, 160, 255);
        SDL_RenderFillRect(ui->renderer, &fill);
    }

    ui_draw_button(ui->renderer,
                   ui->history_minus_button,
                   config->history_seconds > WATERFALL_HISTORY_MIN_SECONDS,
                   false);
    ui_draw_button(ui->renderer,
                   ui->history_plus_button,
                   config->history_seconds < WATERFALL_HISTORY_MAX_SECONDS,
                   true);
}

static int spectrum_y_for_db(SDL_Rect rect, float db, float min_db, float max_db)
{
    float x = (db - min_db) / (max_db - min_db);
    if (x < 0.0f) {
        x = 0.0f;
    } else if (x > 1.0f) {
        x = 1.0f;
    }
    return rect.y + (int)((1.0f - x) * (float)(rect.h - 1) + 0.5f);
}

static void ui_draw_spectrum(ui_t *ui, const waterfall_t *wf, SDL_Rect rect, float min_db, float max_db)
{
    if (rect.w <= 1 || rect.h <= 1) {
        return;
    }

    SDL_SetRenderDrawColor(ui->renderer, 4, 6, 28, 255);
    SDL_RenderFillRect(ui->renderer, &rect);

    SDL_SetRenderDrawColor(ui->renderer, 13, 28, 58, 255);
    for (int i = 1; i < 4; i++) {
        const int y = rect.y + (rect.h * i) / 4;
        SDL_RenderDrawLine(ui->renderer, rect.x, y, rect.x + rect.w - 1, y);
    }
    for (int i = 1; i < 8; i++) {
        const int x = rect.x + (rect.w * i) / 8;
        SDL_RenderDrawLine(ui->renderer, x, rect.y, x, rect.y + rect.h - 1);
    }

    SDL_SetRenderDrawColor(ui->renderer, 28, 55, 96, 255);
    SDL_RenderDrawRect(ui->renderer, &rect);

    int previous_x = rect.x;
    int previous_y = spectrum_y_for_db(rect, wf->spectrum_db[0], min_db, max_db);
    for (int x = 1; x < rect.w; x++) {
        const size_t bin = ((size_t)x * wf->fft_size) / (size_t)rect.w;
        const int screen_x = rect.x + x;
        const int screen_y = spectrum_y_for_db(rect, wf->spectrum_db[bin], min_db, max_db);
        SDL_SetRenderDrawColor(ui->renderer, 43, 178, 224, 255);
        SDL_RenderDrawLine(ui->renderer, previous_x, previous_y, screen_x, screen_y);
        SDL_SetRenderDrawColor(ui->renderer, 10, 54, 91, 100);
        SDL_RenderDrawLine(ui->renderer, screen_x, screen_y + 1, screen_x, rect.y + rect.h - 2);
        previous_x = screen_x;
        previous_y = screen_y;
    }

    const int zero_x = rect.x + rect.w / 2;
    SDL_SetRenderDrawColor(ui->renderer, 94, 206, 172, 255);
    SDL_RenderDrawLine(ui->renderer, zero_x, rect.y, zero_x, rect.y + rect.h - 1);
}

static void ui_update(ui_t *ui, const waterfall_t *wf, const app_config_t *config, const control_state_t *control, const rx_stats_t *stats, struct timespec start)
{
    float min_db = config->min_db;
    float max_db = config->max_db;
    compute_display_range(ui, wf, config, &min_db, &max_db);
    for (size_t row = 0; row < wf->rows; row++) {
        const size_t source_row = (wf->next_row + wf->rows - 1U - row) % wf->rows;
        const float *history_row = wf->history + source_row * wf->fft_size;
        uint32_t *pixel_row = ui->pixels + row * wf->fft_size;
        for (size_t bin = 0; bin < wf->fft_size; bin++) {
            pixel_row[bin] = color_map(history_row[bin], min_db, max_db);
        }
    }
    int window_width = 0;
    int window_height = 0;
    SDL_GetRendererOutputSize(ui->renderer, &window_width, &window_height);
    SDL_UpdateTexture(ui->texture, 0, ui->pixels, (int)(wf->fft_size * sizeof(uint32_t)));
    SDL_SetRenderDrawColor(ui->renderer, 3, 4, 32, 255);
    SDL_RenderClear(ui->renderer);
    const int spectrum_height = spectrum_height_for_window(window_height);
    SDL_Rect spectrum_rect = {
        .x = 0,
        .y = UI_TOOLBAR_HEIGHT,
        .w = window_width,
        .h = spectrum_height,
    };
    const int waterfall_y = UI_TOOLBAR_HEIGHT + spectrum_height;
    SDL_Rect waterfall_rect = {
        .x = 0,
        .y = waterfall_y,
        .w = window_width,
        .h = window_height > waterfall_y ? window_height - waterfall_y : 1,
    };
    SDL_RenderCopy(ui->renderer, ui->texture, 0, &waterfall_rect);
    ui_draw_spectrum(ui, wf, spectrum_rect, min_db, max_db);
    ui_draw_toolbar(ui, config, control, stats, window_width);
    ui_draw_dropdowns(ui, control);
    SDL_RenderPresent(ui->renderer);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    (void)start;
    const uint64_t now_ns = monotonic_ns();
    const double window_s = stats->rate_window_start_ns > 0U && now_ns > stats->rate_window_start_ns
        ? (double)(now_ns - stats->rate_window_start_ns) / 1000000000.0
        : 0.0;
    const double msps = window_s > 0.0 ? (double)stats->rate_window_samples / window_s / 1000000.0 : 0.0;
    char stream_info[96] = "";
    if (stats->stream_center_hz > 0U) {
        snprintf(stream_info,
                 sizeof(stream_info),
                 "f=%.6f MHz bw=%.3f MHz | ",
                 (double)stats->stream_center_hz / 1000000.0,
                 (double)stats->stream_bandwidth_hz / 1000000.0);
    }
    char channel_info[48] = "";
    if (control->enabled) {
        const bool in_window = control_current(control)->in_frontend_window;
        snprintf(channel_info, sizeof(channel_info), "ch=%u | %s", control->channel, in_window ? "" : "OUT OF WINDOW | ");
    }
    char title[420];
    snprintf(title,
             sizeof(title),
             "SDR Waterfall | %s%s%.2f MS/s | hist=%.1fs stride=%zu | packets=%llu frames=%llu gaps=%llu bad=%llu stream=0x%08x levels=%.1f..%.1f dB row=%.1f..%.1f dB",
             channel_info,
             stream_info,
             msps,
             config->history_seconds,
             config->frame_stride_samples,
             (unsigned long long)stats->packets,
             (unsigned long long)stats->frames,
             (unsigned long long)stats->sequence_gaps,
             (unsigned long long)stats->bad_packets,
             stats->last_stream_id,
             (double)min_db,
             (double)max_db,
             (double)wf->last_min_db,
             (double)wf->last_max_db);
    SDL_SetWindowTitle(ui->window, title);
}

int main(int argc, char **argv)
{
    app_config_t config;
    if (!parse_args(argc, argv, &config)) {
        usage(argv[0]);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    control_state_t control;
    memset(&control, 0, sizeof(control));
    if (config.control_url != 0) {
        char control_error[CONTROL_MAX_ERROR];
        if (!control_client_init(&control.client, config.control_url, control_error, sizeof(control_error))) {
            fprintf(stderr, "control: %s\n", control_error);
            return 2;
        }
        control.enabled = true;
        /* When the simulator publishes to a multicast group, receive on that group
         * unless the user chose a bind host explicitly. */
        struct in_addr group_addr;
        if (strcmp(config.host, "0.0.0.0") == 0 &&
            inet_pton(AF_INET, control.client.udp_output_host, &group_addr) == 1 &&
            ipv4_is_multicast(group_addr)) {
            snprintf(control.multicast_host, sizeof(control.multicast_host), "%s", control.client.udp_output_host);
            config.host = control.multicast_host;
        }
        if (!control_select_channel(&config, &control, 0, 0, 0, 0, config.channel_id)) {
            return 2;
        }
        /* The channel's rate may equal the default, in which case apply_sample_rate was a
         * no-op; make sure the stride matches the discovered rate either way. */
        config.frame_stride_samples = waterfall_stride_for_history(config.rows, config.sample_rate_hz, config.history_seconds, config.fft_size);
        config.history_seconds = waterfall_history_for_stride(config.rows, config.sample_rate_hz, config.frame_stride_samples);
    }

    waterfall_t waterfall;
    if (!waterfall_init(&waterfall, config.fft_size, config.rows)) {
        fprintf(stderr, "failed to allocate waterfall buffers\n");
        return 2;
    }
    int fd = open_udp_socket(&config);
    if (fd < 0) {
        fprintf(stderr, "failed to bind UDP %s:%u: %s\n", config.host, config.port, strerror(errno));
        waterfall_free(&waterfall);
        return 3;
    }

    ui_t ui;
    const bool use_ui = !config.headless;
    if (use_ui && !ui_init(&ui, &config)) {
        close(fd);
        waterfall_free(&waterfall);
        return 4;
    }

#ifdef __linux__
    rx_batch_t rx_batch;
    if (!rx_batch_init(&rx_batch)) {
        if (use_ui) {
            ui_free(&ui);
        }
        close(fd);
        waterfall_free(&waterfall);
        return 5;
    }
#else
    uint8_t *packet_buffer = malloc(MAX_PACKET_BYTES);
    if (packet_buffer == 0) {
        if (use_ui) {
            ui_free(&ui);
        }
        close(fd);
        waterfall_free(&waterfall);
        return 5;
    }
#endif

    frame_sampler_t sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.iq = calloc(2U * config.fft_size, sizeof(*sampler.iq));
    if (sampler.iq == 0) {
#ifdef __linux__
        rx_batch_free(&rx_batch);
#else
        free(packet_buffer);
#endif
        if (use_ui) {
            ui_free(&ui);
        }
        close(fd);
        waterfall_free(&waterfall);
        return 5;
    }

    rx_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (control.enabled) {
        stats.stream_center_hz = control_current(&control)->center_frequency_hz;
        stats.stream_bandwidth_hz = control_current(&control)->bandwidth_hz;
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const int receive_buffer_bytes = socket_receive_buffer_bytes(fd);
    fprintf(stderr,
            "listening on %s:%u fft=%zu rows=%zu history=%.1fs stride=%zu samples rcvbuf=%d bytes\n",
            config.host,
            config.port,
            config.fft_size,
            config.rows,
            config.history_seconds,
            config.frame_stride_samples,
            receive_buffer_bytes);
    print_receive_buffer_warning(receive_buffer_bytes);

    while (keep_running) {
        bool frame_ready = false;
        bool ui_needs_redraw = false;
        if (use_ui) {
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0) {
                if (event.type == SDL_QUIT) {
                    keep_running = 0;
                } else if (event.type == SDL_WINDOWEVENT &&
                           event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
                           config.auto_rows) {
                    const size_t rows = rows_for_window_height(event.window.data2);
                    if (!set_waterfall_rows(&waterfall, &ui, &config, rows, &sampler)) {
                        keep_running = 0;
                    }
                    ui_needs_redraw = true;
                } else if (event.type == SDL_TEXTINPUT && (ui.freq_editing || ui.rx_freq_editing)) {
                    if (ui.freq_editing) {
                        freq_text_append(ui.freq_edit, sizeof(ui.freq_edit), event.text.text);
                    } else {
                        freq_text_append(ui.rx_freq_edit, sizeof(ui.rx_freq_edit), event.text.text);
                    }
                    ui_needs_redraw = true;
                } else if (event.type == SDL_KEYDOWN && (ui.freq_editing || ui.rx_freq_editing)) {
                    /* A frequency field swallows all keys while editing, so digits and
                     * `B` do not double as channel/bandwidth shortcuts. */
                    const SDL_Keycode sym = event.key.keysym.sym;
                    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        if (ui.freq_editing) {
                            freq_edit_commit(&ui, &control, &stats);
                        } else {
                            rx_freq_edit_commit(&ui, &control, &stats);
                        }
                    } else if (sym == SDLK_ESCAPE) {
                        freq_edit_cancel(&ui);
                        rx_freq_edit_cancel(&ui);
                    } else if (sym == SDLK_BACKSPACE) {
                        freq_text_backspace(ui.freq_editing ? ui.freq_edit : ui.rx_freq_edit);
                    }
                    ui_needs_redraw = true;
                } else if (event.type == SDL_KEYDOWN) {
                    const SDL_Keycode sym = event.key.keysym.sym;
                    const bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                    if (sym == SDLK_LEFTBRACKET || sym == SDLK_MINUS) {
                        set_history_seconds(&config, config.history_seconds - 1.0, &sampler);
                        ui_needs_redraw = true;
                    } else if (sym == SDLK_RIGHTBRACKET || sym == SDLK_EQUALS || sym == SDLK_PLUS) {
                        set_history_seconds(&config, config.history_seconds + 1.0, &sampler);
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym == SDLK_ESCAPE && ui.open_combo != UI_COMBO_NONE) {
                        ui.open_combo = UI_COMBO_NONE;
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym == SDLK_PAGEUP && control.channel > 0U) {
                        control_select_channel(&config, &control, &fd, &waterfall, &sampler, &stats, control.channel - 1U);
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym == SDLK_PAGEDOWN) {
                        control_select_channel(&config, &control, &fd, &waterfall, &sampler, &stats, control.channel + 1U);
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym >= SDLK_0 && sym <= SDLK_9) {
                        control_select_channel(&config, &control, &fd, &waterfall, &sampler, &stats, (uint32_t)(sym - SDLK_0));
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym == SDLK_LEFT) {
                        control_step_frequency(&control, &stats, -1, shift);
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym == SDLK_RIGHT) {
                        control_step_frequency(&control, &stats, 1, shift);
                        ui_needs_redraw = true;
                    } else if (control.enabled && sym == SDLK_b) {
                        control_cycle_bandwidth(&config, &control, &waterfall, &sampler, &stats);
                        ui_needs_redraw = true;
                    }
                } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    int window_width = 0;
                    int window_height = 0;
                    SDL_GetRendererOutputSize(ui.renderer, &window_width, &window_height);
                    (void)window_height;
                    ui_layout(&ui, window_width);
                    const int mx = event.button.x;
                    const int my = event.button.y;
                    ui_needs_redraw = true;
                    if (control.enabled && ui.open_combo == UI_COMBO_CHANNEL) {
                        /* An open dropdown captures the click: select an item or close. */
                        ui.open_combo = UI_COMBO_NONE;
                        for (size_t i = 0; i < control.client.channel_count; i++) {
                            const SDL_Rect item = ui_dropdown_item_rect(ui.channel_combo.x, i);
                            if (ui_point_in_rect(mx, my, &item)) {
                                control_select_channel(&config, &control, &fd, &waterfall, &sampler, &stats, (uint32_t)i);
                                break;
                            }
                        }
                    } else if (control.enabled && ui.open_combo == UI_COMBO_BANDWIDTH) {
                        ui.open_combo = UI_COMBO_NONE;
                        uint32_t options[CONTROL_MAX_PROFILES];
                        const size_t count = control_bandwidth_options(&control, options, CONTROL_MAX_PROFILES);
                        for (size_t i = 0; i < count; i++) {
                            const SDL_Rect item = ui_dropdown_item_rect(ui.bandwidth_combo.x, i);
                            if (ui_point_in_rect(mx, my, &item)) {
                                control_set_bandwidth(&config, &control, &waterfall, &sampler, &stats, options[i]);
                                break;
                            }
                        }
                    } else if (control.enabled && ui.freq_editing && !ui_point_in_rect(mx, my, &ui.freq_field)) {
                        freq_edit_cancel(&ui);
                    } else if (control.enabled && ui.rx_freq_editing && !ui_point_in_rect(mx, my, &ui.rx_freq_field)) {
                        rx_freq_edit_cancel(&ui);
                    } else if (ui_point_in_rect(mx, my, &ui.history_minus_button)) {
                        set_history_seconds(&config, config.history_seconds - 1.0, &sampler);
                    } else if (ui_point_in_rect(mx, my, &ui.history_plus_button)) {
                        set_history_seconds(&config, config.history_seconds + 1.0, &sampler);
                    } else if (control.enabled && ui_point_in_rect(mx, my, &ui.rx_freq_field)) {
                        if (!ui.rx_freq_editing) {
                            rx_freq_edit_begin(&ui, &control);
                        }
                    } else if (control.enabled && ui_point_in_rect(mx, my, &ui.channel_combo)) {
                        freq_edit_cancel(&ui);
                        rx_freq_edit_cancel(&ui);
                        ui.open_combo = UI_COMBO_CHANNEL;
                    } else if (control.enabled && ui_point_in_rect(mx, my, &ui.bandwidth_combo)) {
                        freq_edit_cancel(&ui);
                        rx_freq_edit_cancel(&ui);
                        ui.open_combo = UI_COMBO_BANDWIDTH;
                    } else if (control.enabled && ui_point_in_rect(mx, my, &ui.freq_field)) {
                        if (!ui.freq_editing) {
                            freq_edit_begin(&ui, &control, &stats);
                        }
                    }
                }
            }
        }

        for (size_t drained = 0; drained < 4096U;) {
#ifdef __linux__
            const unsigned int batch_limit = (4096U - drained) < RX_BATCH_SIZE ? (unsigned int)(4096U - drained) : RX_BATCH_SIZE;
            const int received = recvmmsg(fd, rx_batch.messages, batch_limit, MSG_DONTWAIT, 0);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "recvmmsg failed: %s\n", strerror(errno));
                keep_running = 0;
                break;
            }
            if (received == 0) {
                break;
            }
            drained += (size_t)received;
            for (int i = 0; i < received; i++) {
                const uint8_t *data = rx_batch.storage + (size_t)i * MAX_PACKET_BYTES;
                if (handle_received_packet(&config,
                                           &waterfall,
                                           &stats,
                                           &sampler,
                                           data,
                                           rx_batch.messages[i].msg_len,
                                           &frame_ready)) {
                    keep_running = 0;
                    break;
                }
                rx_batch.messages[i].msg_len = 0;
            }
            if (!keep_running || received < (int)batch_limit) {
                break;
            }
#else
            const ssize_t n = recv(fd, packet_buffer, MAX_PACKET_BYTES, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "recv failed: %s\n", strerror(errno));
                keep_running = 0;
                break;
            }
            drained++;
            if (handle_received_packet(&config,
                                       &waterfall,
                                       &stats,
                                       &sampler,
                                       packet_buffer,
                                       (size_t)n,
                                       &frame_ready)) {
                keep_running = 0;
                break;
            }
#endif
        }

        if (control_sync_with_stream(&control, &stats)) {
            ui_needs_redraw = true;
        }
        if (use_ui && (frame_ready || ui_needs_redraw)) {
            ui_update(&ui, &waterfall, &config, &control, &stats, start);
        } else if (!use_ui && frame_ready) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            print_status(&stats, &waterfall, elapsed_seconds(start, now));
        }
        if ((config.max_packets > 0 && stats.packets >= config.max_packets) ||
            (config.max_frames > 0 && stats.frames >= config.max_frames)) {
            break;
        }
        if (use_ui) {
            SDL_Delay(1);
        } else {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
            nanosleep(&ts, 0);
        }
    }

    struct timespec stop;
    clock_gettime(CLOCK_MONOTONIC, &stop);
    print_status(&stats, &waterfall, elapsed_seconds(start, stop));
    free(sampler.iq);
#ifdef __linux__
    rx_batch_free(&rx_batch);
#else
    free(packet_buffer);
#endif
    if (use_ui) {
        ui_free(&ui);
    }
    close(fd);
    waterfall_free(&waterfall);
    return stats.bad_packets == 0 ? 0 : 1;
}
