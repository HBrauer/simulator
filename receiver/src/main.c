#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "history.h"
#include "vita49_rx.h"
#include "waterfall.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
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
#define UI_BUTTON_SIZE 32
#define UI_BUTTON_MARGIN 8
#define AUTO_LEVEL_DYNAMIC_RANGE_DB 90.0f
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
} app_config_t;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t payload_samples;
    uint64_t frames;
    uint64_t bad_packets;
    uint64_t sequence_gaps;
    bool have_sequence;
    uint8_t last_sequence;
    uint32_t last_stream_id;
    uint64_t last_timestamp_ns;
} rx_stats_t;

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

static size_t rows_for_window_height(int height)
{
    const int rows = height - UI_TOOLBAR_HEIGHT;
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
            "  --headless                  Receive/process without opening SDL window\n",
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
    if (config->port == 0 || config->sample_rate_hz == 0 || !waterfall_fft_size_valid(config->fft_size) || config->rows == 0) {
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
    const app_config_t *config,
    waterfall_t *waterfall,
    rx_stats_t *stats,
    frame_sampler_t *sampler,
    const uint8_t *packet_data,
    size_t packet_bytes,
    bool *frame_ready)
{
    vita49_rx_packet_t packet;
    if (!vita49_rx_parse_if_data(packet_data, packet_bytes, &packet)) {
        stats->bad_packets++;
        return false;
    }
    update_sequence_stats(stats, &packet);
    stats->packets++;
    stats->bytes += (uint64_t)packet_bytes;
    stats->payload_samples += (uint64_t)(packet.payload_bytes / 4U);
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
} ui_t;

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

static void ui_draw_toolbar(ui_t *ui, const app_config_t *config, int window_width)
{
    ui_layout(ui, window_width);

    SDL_Rect toolbar = {.x = 0, .y = 0, .w = window_width, .h = UI_TOOLBAR_HEIGHT};
    SDL_SetRenderDrawColor(ui->renderer, 17, 22, 28, 255);
    SDL_RenderFillRect(ui->renderer, &toolbar);

    const int bar_x = UI_BUTTON_MARGIN;
    const int bar_y = UI_TOOLBAR_HEIGHT / 2 - 4;
    const int bar_w = ui->history_minus_button.x - (2 * UI_BUTTON_MARGIN);
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

static void ui_update(ui_t *ui, const waterfall_t *wf, const app_config_t *config, const rx_stats_t *stats, struct timespec start)
{
    float min_db = config->min_db;
    float max_db = config->max_db;
    compute_display_range(ui, wf, config, &min_db, &max_db);
    for (size_t row = 0; row < wf->rows; row++) {
        const size_t source_row = (wf->next_row + row) % wf->rows;
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
    SDL_Rect waterfall_rect = {
        .x = 0,
        .y = UI_TOOLBAR_HEIGHT,
        .w = window_width,
        .h = window_height > UI_TOOLBAR_HEIGHT ? window_height - UI_TOOLBAR_HEIGHT : window_height,
    };
    SDL_RenderCopy(ui->renderer, ui->texture, 0, &waterfall_rect);
    ui_draw_toolbar(ui, config, window_width);
    SDL_RenderPresent(ui->renderer);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double seconds = elapsed_seconds(start, now);
    const double msps = seconds > 0.0 ? (double)stats->payload_samples / seconds / 1000000.0 : 0.0;
    char title[320];
    snprintf(title,
             sizeof(title),
             "SDR Waterfall | %.2f MS/s | hist=%.1fs stride=%zu | packets=%llu frames=%llu gaps=%llu bad=%llu stream=0x%08x levels=%.1f..%.1f dB row=%.1f..%.1f dB",
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

    waterfall_t waterfall;
    if (!waterfall_init(&waterfall, config.fft_size, config.rows)) {
        fprintf(stderr, "failed to allocate waterfall buffers\n");
        return 2;
    }
    const int fd = open_udp_socket(&config);
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
                } else if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_LEFTBRACKET || event.key.keysym.sym == SDLK_MINUS) {
                        set_history_seconds(&config, config.history_seconds - 1.0, &sampler);
                        ui_needs_redraw = true;
                    } else if (event.key.keysym.sym == SDLK_RIGHTBRACKET ||
                               event.key.keysym.sym == SDLK_EQUALS ||
                               event.key.keysym.sym == SDLK_PLUS) {
                        set_history_seconds(&config, config.history_seconds + 1.0, &sampler);
                        ui_needs_redraw = true;
                    }
                } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    int window_width = 0;
                    int window_height = 0;
                    SDL_GetRendererOutputSize(ui.renderer, &window_width, &window_height);
                    (void)window_height;
                    ui_layout(&ui, window_width);
                    if (ui_point_in_rect(event.button.x, event.button.y, &ui.history_minus_button)) {
                        set_history_seconds(&config, config.history_seconds - 1.0, &sampler);
                        ui_needs_redraw = true;
                    } else if (ui_point_in_rect(event.button.x, event.button.y, &ui.history_plus_button)) {
                        set_history_seconds(&config, config.history_seconds + 1.0, &sampler);
                        ui_needs_redraw = true;
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

        if (use_ui && (frame_ready || ui_needs_redraw)) {
            ui_update(&ui, &waterfall, &config, &stats, start);
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
