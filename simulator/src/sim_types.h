#ifndef SIM_TYPES_H
#define SIM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_MAX_RECEIVERS 12
/* Sized for the wideband-plus-many-narrow-DDC use case: one track_tuner channel and ~20
 * fixed DDC channels extracting from the same recording. */
#define SIM_MAX_CHANNELS 24
#define SIM_MAX_CHANNEL_RATES 128
/* Upper bound on per-channel cooperative render threads (channel_config_t.render_threads). */
#define SIM_MAX_RENDER_THREADS 16U
#define SIM_MAX_SOURCES 64
#define SIM_MAX_SIGNALS 256
#define SIM_MAX_PASSTHROUGH_VARIANTS 8
#define SIM_MAX_PATH 256
#define SIM_MAX_ID 64
#define SIM_RECEIVER_BANDWIDTH_HZ 80000000ULL
#define SIM_RECEIVER_SAMPLE_RATE_HZ 98304000U
#define SIM_DDC_BANDWIDTH_HZ 20000000U
#define SIM_DDC_SAMPLE_RATE_HZ 24576000U
#define SIM_MAX_RF_HZ 100000000000ULL
#define SIM_DEFAULT_STREAM_BLOCK_SAMPLES 1024U
#define SIM_MAX_STREAM_BLOCK_SAMPLES 4096U
/* Upper bound on the wall-clock span of samples the UDP thread lumps into a single paced
 * sendmmsg burst. Batching amortises the syscall at high rates (80 MHz), but pacing a full
 * batch as one unit turns into visible chop at low rates where 16 blocks span hundreds of ms;
 * this caps the batch by time so low-rate streams update smoothly. See streamer.c. */
#define SIM_DEFAULT_STREAM_MAX_BATCH_LATENCY_US 25000ULL

/* A signal with neither power_dbm nor snr_db configured is placed this many dB above the
 * in-band noise floor. Because the margin is relative to the noise, one fixed value keeps a
 * signal comfortably visible at any bandwidth or noise level -- no hand-computed dBm needed. */
#define SIM_DEFAULT_SIGNAL_SNR_DB 20.0
/* Reference noise density used to resolve snr_db (and the default SNR) when the noise floor is
 * disabled. With no actual noise the signal is visible regardless; this only fixes the absolute
 * power the SNR margin is measured from, so behaviour is continuous when noise is toggled off. */
#define SIM_NOISE_FLOOR_DISABLED_DENSITY_DBM_PER_HZ -110.0

typedef struct {
    int16_t i;
    int16_t q;
} iq_ci16_t;

/* Wide internal sample types carried end to end (renderer -> ringbuffer -> wire) for the
 * non-CI16 output formats, so ci24/cf32 keep the render pipeline's sub-16-bit precision
 * instead of being quantised to int16 first. */
typedef struct {
    int32_t i; /* 24-bit signed value, range [-2^23, 2^23-1]; left-justified only on the wire */
    int32_t q;
} iq_ci24_t;

typedef struct {
    float i; /* full scale +-1.0 (0 dBFS); IEEE-754 range, not clamped, per VITA 49.2 6.1.1.4 */
    float q;
} iq_cf32_t;

/* Internal mixer source sample: complex float in *int16-scale* units (full-scale magnitude
 * ~32768), the lossless widening of the former iq_ci16_t source storage. Cached IQ sources, the
 * audio pre-renders, and the DDC intermediate are all carried in this type so a float (cf32)
 * source keeps full precision -- the old ci16 storage quantised sub-LSB float content to zero and
 * clipped overrange at load. A ci16 file widens exactly ((float)n); a cf32 file scales by 2^15
 * with no rounding. Every downstream gain constant is unchanged because the scale is identical to
 * the old int16 counts. Distinct from iq_cf32_t, which is the +-1.0 on-wire output format. */
typedef struct {
    float i;
    float q;
} iq_src_t;

/* On-wire sample format of a channel's VITA 49.2 IF-data payload, which also selects the
 * channel's internal sample type. All formats are big-endian, processing-efficient, Complex
 * Cartesian per VITA 49.2 (see docs/vita49_udp.md).
 *   CI16 - 16-bit signed fixed-point, 16-bit item packing field (4 bytes/sample).
 *   CI24 - 24-bit signed fixed-point, left-justified in a 32-bit item packing field (8 bytes/sample).
 *   CF32 - IEEE-754 single-precision float, full scale +-1.0 (8 bytes/sample). */
typedef enum {
    SIM_OUTPUT_FORMAT_CI16,
    SIM_OUTPUT_FORMAT_CI24,
    SIM_OUTPUT_FORMAT_CF32
} sim_output_format_t;

/* Bytes per complex sample of a format's internal storage and its on-wire payload (they are
 * equal: 4 for CI16, 8 for CI24 and CF32). Defined in vita49_packet.c. */
size_t sim_internal_bytes_per_sample(sim_output_format_t format);

/* Map a format name ("ci16"/"ci24"/"cf32") to its enum. Returns false for any other string. */
bool sim_output_format_from_name(const char *name, sim_output_format_t *out);

typedef enum {
    SCENARIO_SOURCE_IQ_FILE,
    SCENARIO_SOURCE_AUDIO_FILE
} scenario_source_kind_t;

/* How a signal is placed against the channel tune (see docs/schemas.md):
 * FIXED  - at its own center_frequency_hz whenever the passband overlaps (legacy behaviour).
 * RANGE  - centered at the tuned frequency while the tune is inside [range_start, range_stop];
 *          output is identical anywhere in the range, silent outside it.
 * SHIFT  - stays at its absolute RF position: IQ rotated by f0 - f_tune while the tune is
 *          inside the range (pure rotation; spectral wrap-around is accepted semantics). */
typedef enum {
    SCENARIO_REPLAY_FIXED,
    SCENARIO_REPLAY_RANGE,
    SCENARIO_REPLAY_SHIFT
} scenario_replay_mode_t;

typedef enum {
    SCENARIO_MODULATION_IQ,
    SCENARIO_MODULATION_WBFM,
    SCENARIO_MODULATION_AM,
    SCENARIO_MODULATION_USB,
    SCENARIO_MODULATION_LSB
} scenario_modulation_t;

typedef struct {
    uint16_t port;
} udp_output_config_t;

/* A supported {bandwidth, sample rate} option a channel can be tuned to. Validation enforces
 * sample_rate_hz >= bandwidth_hz > 0 and bandwidth_hz <= the receiver's front-end bandwidth_hz. */
typedef struct {
    uint32_t bandwidth_hz;
    uint32_t sample_rate_hz;
} channel_rate_t;

/* One output channel of a receiver. Every stream is a channel; the former wideband stream
 * is just a channel with track_tuner=true and the widest bandwidth. Each channel carries a list
 * of supported {bandwidth, sample rate} options in `rates`; the first is active at load, and REST
 * retunes/select among them. bandwidth_hz/sample_rate_hz denormalise the currently active rate. */
typedef struct {
    uint32_t id;
    bool track_tuner;             /* center follows the receiver tuner (scan or fixed) */
    uint64_t center_frequency_hz; /* used when !track_tuner */
    uint32_t bandwidth_hz;        /* active; denormalised from the selected entry of rates[] */
    uint32_t sample_rate_hz;      /* active; denormalised from the selected entry of rates[] */
    size_t rate_count;
    channel_rate_t rates[SIM_MAX_CHANNEL_RATES];
    double output_scale;
    double rf_reference_power_dbm;
    bool stream_enabled;
    /* VITA 49 Stream ID carried in every data/context packet for this channel. When
     * stream_id_set is false, config_validate auto-assigns a value by counting up across all
     * channels (0, 1, 2, ...) in receiver/channel order. */
    bool stream_id_set;
    uint32_t stream_id;
    /* Number of CPU threads that cooperatively render this channel's blocks. 1 (default) keeps
     * the single-thread render path. A wideband synthesis channel (e.g. 80 MHz / 98.304 MS/s
     * mixing several signals) exceeds one core; raising this renders several consecutive blocks
     * in parallel so the stream sustains real time. Clamped to [1, SIM_MAX_RENDER_THREADS]. */
    uint32_t render_threads;
    /* On-wire VITA 49.2 payload sample format for this channel. Defaults to CI16. */
    sim_output_format_t output_format;
    udp_output_config_t udp_output;
} channel_config_t;

typedef struct {
    uint32_t id;
    char rest_bind_host[64];
    uint16_t rest_port;
    uint64_t frequency_min_hz;
    uint64_t frequency_max_hz;
    uint64_t bandwidth_hz; /* instantaneous analog (ADC) window all channels extract from */
    double scan_rate_hz_per_s;
    double output_scale;
    double rf_reference_power_dbm;
    char udp_output_host[64];
    char udp_multicast_interface[64];
    uint64_t config_epoch; /* bumped under the receiver lock on every runtime change */
    size_t channel_count;
    channel_config_t channels[SIM_MAX_CHANNELS];
} receiver_config_t;

#define SIM_MAX_STREAM_CPUS 64

typedef struct {
    int schema_version;
    char instance_id[SIM_MAX_ID];
    char scenario_file[SIM_MAX_PATH];
    char log_path[SIM_MAX_PATH];
    size_t stream_block_samples;
    uint64_t stream_max_batch_latency_us; /* time cap on a paced UDP send batch; 0 -> default */
    int stream_cpu;                          /* legacy single-CPU alias; -1 = unset */
    int stream_cpus[SIM_MAX_STREAM_CPUS];    /* CPUs the stream threads are spread across */
    size_t stream_cpu_count;                 /* 0 = no pinning */
    size_t asset_cache_max_bytes;
    size_t ddc_cache_max_bytes; /* budget for precomputed DDC intermediates (0 disables caching) */
    double audio_prerender_oversample;    /* oversampling factor over content bandwidth (default 2.0) */
    uint32_t audio_prerender_max_rate_hz; /* ceiling on the intermediate pre-render rate (default 4 MHz) */
    /* Optional VITA 49 Class ID emitted in every data/context packet. Enabled when
     * class_id_present is set (via any class_id_* config key). */
    bool class_id_present;
    uint32_t class_id_oui; /* 24-bit OUI */
    uint16_t class_id_information_code;
    uint16_t class_id_packet_code;
    size_t receiver_count;
    receiver_config_t receivers[SIM_MAX_RECEIVERS];
} simulator_config_t;

/* One rate-specific capture of a passthrough source. A passthrough channel selects the variant
 * whose sample_rate_hz matches its own rate and streams it verbatim; a channel bandwidth with no
 * matching variant renders silence (passthrough never resamples). */
typedef struct {
    char file[SIM_MAX_PATH];
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint64_t sample_count; /* filled in from the file at validation */
} scenario_passthrough_variant_t;

typedef struct {
    char id[SIM_MAX_ID];
    char source_type[32];
    scenario_source_kind_t source_kind;
    char file[SIM_MAX_PATH];
    char format[16];
    char byte_order[32];
    char iq_layout[32];
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    int64_t center_frequency_hz;
    uint64_t sample_count;
    double nominal_level_dbfs;
    /* Optional: a passthrough source carries one capture per bandwidth here instead of a single
     * top-level `file`. When non-empty, `file`/`sample_rate_hz` are unused for replay. */
    size_t passthrough_variant_count;
    scenario_passthrough_variant_t passthrough_variants[SIM_MAX_PASSTHROUGH_VARIANTS];
} scenario_source_t;

typedef struct {
    char signal_id[SIM_MAX_ID];
    char source_reference[SIM_MAX_ID];
    char modulation_name[16];
    scenario_modulation_t modulation;
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    double power_dbm;         /* absolute transmit power; resolved at load from snr_db (or the
                               * default SNR) when not given explicitly, so it is always set here */
    double fm_deviation_hz;
    double am_depth;
    double start_time_s;
    double repeat_interval_s;
    scenario_replay_mode_t replay_mode;
    uint64_t replay_range_start_hz;
    uint64_t replay_range_stop_hz;
    bool loop; /* derived, not configured: true iff repeat_interval_s is absent. Selects
                * continuous epoch-anchored looping over the start/repeat burst model. */
    /* When active, this channel bypasses the mixer entirely for the block: no float mix bus,
     * no noise floor, no other signals -- just this source's samples (optionally gain-scaled
     * and/or frequency-rotated for shift mode) written straight to the output. Requires
     * replay_mode range/shift and loop true; only engages on a per-block basis when the
     * channel's sample rate exactly matches the source's (no resampling on this path). */
    bool passthrough;
} scenario_signal_t;

typedef struct {
    bool enabled;
    double power_dbm_per_hz;  /* noise power spectral density (total in-window power scales with bandwidth) */
    uint64_t seed;
} scenario_noise_floor_t;

typedef struct {
    int schema_version;
    char scenario_id[SIM_MAX_ID];
    char description[256];
    scenario_noise_floor_t noise_floor;
    size_t source_count;
    scenario_source_t sources[SIM_MAX_SOURCES];
    size_t signal_count;
    scenario_signal_t signals[SIM_MAX_SIGNALS];
} scenario_t;

typedef enum {
    RECEIVER_MODE_FIXED,
    RECEIVER_MODE_SCAN
} receiver_mode_t;

#endif
