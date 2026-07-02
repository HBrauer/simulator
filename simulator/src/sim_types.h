#ifndef SIM_TYPES_H
#define SIM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_MAX_RECEIVERS 12
#define SIM_DDC_COUNT 4
#define SIM_MAX_SOURCES 64
#define SIM_MAX_SIGNALS 256
#define SIM_MAX_PATH 256
#define SIM_MAX_ID 64
#define SIM_RECEIVER_BANDWIDTH_HZ 80000000ULL
#define SIM_RECEIVER_SAMPLE_RATE_HZ 98304000U
#define SIM_DDC_BANDWIDTH_HZ 20000000U
#define SIM_DDC_SAMPLE_RATE_HZ 24576000U
#define SIM_MAX_RF_HZ 40000000000ULL
#define SIM_DEFAULT_STREAM_BLOCK_SAMPLES 1024U
#define SIM_MAX_STREAM_BLOCK_SAMPLES 4096U

typedef struct {
    int16_t i;
    int16_t q;
} iq_ci16_t;

typedef struct {
    uint16_t port;
} udp_output_config_t;

typedef struct {
    uint32_t id;
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    uint32_t sample_rate_hz;
    double output_scale;
    double rf_reference_power_dbm;
    bool stream_enabled;
    udp_output_config_t udp_output;
} ddc_config_t;

typedef struct {
    uint32_t id;
    char rest_bind_host[64];
    uint16_t rest_port;
    uint64_t frequency_start_hz;
    uint64_t frequency_stop_hz;
    uint64_t bandwidth_hz;
    uint32_t sample_rate_hz;
    double scan_rate_hz_per_s;
    double output_scale;
    double rf_reference_power_dbm;
    bool stream_enabled;
    char udp_output_host[64];
    udp_output_config_t udp_80mhz_output;
    ddc_config_t ddc[SIM_DDC_COUNT];
} receiver_config_t;

typedef struct {
    int schema_version;
    char instance_id[SIM_MAX_ID];
    char scenario_file[SIM_MAX_PATH];
    char log_path[SIM_MAX_PATH];
    size_t stream_block_samples;
    int stream_cpu;
    size_t asset_cache_max_bytes;
    size_t receiver_count;
    receiver_config_t receivers[SIM_MAX_RECEIVERS];
} simulator_config_t;

typedef struct {
    char id[SIM_MAX_ID];
    char source_type[32];
    char file[SIM_MAX_PATH];
    char format[16];
    char byte_order[32];
    char iq_layout[32];
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    int64_t center_frequency_hz;
    uint64_t sample_count;
    double nominal_level_dbfs;
} scenario_source_t;

typedef struct {
    char signal_id[SIM_MAX_ID];
    char source_reference[SIM_MAX_ID];
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    double power_dbm;
    double start_time_s;
    double repeat_interval_s;
} scenario_signal_t;

typedef struct {
    int schema_version;
    char scenario_id[SIM_MAX_ID];
    char description[256];
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
