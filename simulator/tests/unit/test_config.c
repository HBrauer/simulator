#include "config.h"
#include "receiver.h"
#include "test_suites.h"

#include <check.h>
#include <stdio.h>
#include <unistd.h>

static const char *write_temp_config_body(const char *top_body, const char *receiver_body)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/sim_cfg_%d.yaml", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NULL;
    }
    fputs("schema_version: 1\n", f);
    fputs("instance_id: \"t\"\n", f);
    fputs("scenario_file: \"simulator/scenarios/scanner_fsk.json\"\n", f);
    fputs(top_body, f);
    fputs("receivers:\n"
          "  - receiver_id: 0\n"
          "    rest_bind_host: \"127.0.0.1\"\n"
          "    rest_port: 8100\n"
          "    udp_output_host: \"127.0.0.1\"\n"
          "    frequency_start_hz: 9960000000\n"
          "    frequency_stop_hz: 10040000000\n",
          f);
    fputs(receiver_body, f);
    fclose(f);
    return path;
}

static const char *default_channels_yaml(void)
{
    return "    channels:\n"
           "      - { channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: 50000 }\n"
           "      - { channel_id: 1, center_frequency_hz: 10000000000, bandwidth_hz: 20000000, udp_output_port: 50001 }\n"
           "      - { channel_id: 2, center_frequency_hz: 10001000000, bandwidth_hz: 20000000, udp_output_port: 50002 }\n";
}

static const char *write_temp_config(const char *body)
{
    return write_temp_config_body(body, default_channels_yaml());
}

static receiver_config_t valid_receiver(uint32_t id, uint16_t rest_port, uint16_t udp_base)
{
    receiver_config_t receiver = {
        .id = id,
        .rest_port = rest_port,
        .frequency_start_hz = 9960000000ULL,
        .frequency_stop_hz = 10040000000ULL,
        .frontend_bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ,
        .scan_rate_hz_per_s = 100000000000.0,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -55.0,
        .channel_count = 5,
    };
    receiver_default_profiles(&receiver);
    receiver.channels[0] = (channel_config_t){
        .id = 0,
        .track_tuner = true,
        .bandwidth_hz = (uint32_t)SIM_RECEIVER_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -55.0,
        .stream_enabled = true,
        .udp_output = {.port = udp_base},
    };
    for (size_t i = 1; i < 5; i++) {
        receiver.channels[i] = (channel_config_t){
            .id = (uint32_t)i,
            .center_frequency_hz = 10000000000ULL + (uint64_t)i * 1000000ULL,
            .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
            .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
            .output_scale = 1.0,
            .rf_reference_power_dbm = -55.0,
            .stream_enabled = true,
            .udp_output = {.port = (uint16_t)(udp_base + i)},
        };
    }
    return receiver;
}

START_TEST(loads_instance_config)
{
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml("simulator/configs/receiver_scanner.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.receiver_count, 1);
    ck_assert_uint_eq(config.receivers[0].id, 0);
    ck_assert_uint_eq(config.receivers[0].rest_port, 8100);
    ck_assert_uint_eq(config.receivers[0].channel_count, 5);
    ck_assert_uint_eq(config.receivers[0].channels[4].udp_output.port, 50004);
    ck_assert_uint_eq(config.stream_block_samples, 1024);
    ck_assert_int_eq(config.stream_cpu, -1);
    ck_assert_uint_eq(config.stream_cpu_count, 0); /* absent -> no pinning */
    ck_assert_uint_eq(config.asset_cache_max_bytes, 0);
    ck_assert_uint_eq(config.receivers[0].frontend_bandwidth_hz, 80000000ULL);
    ck_assert(config.receivers[0].channels[0].track_tuner);
    ck_assert_uint_eq(config.receivers[0].channels[0].bandwidth_hz, 80000000U);
    ck_assert_uint_eq(config.receivers[0].channels[0].sample_rate_hz, 98304000U);
    ck_assert(!config.receivers[0].channels[1].track_tuner);
    ck_assert_uint_eq(config.receivers[0].channels[1].bandwidth_hz, 20000000U);
    ck_assert_uint_eq(config.receivers[0].channels[1].sample_rate_hz, 24576000U);
    ck_assert_uint_eq(config.receivers[0].profile_count, 6);
    ck_assert_str_eq(config.receivers[0].profiles[0].name, "80M");
    ck_assert_double_eq_tol(config.receivers[0].rf_reference_power_dbm, -55.0, 0.000001);
    ck_assert_double_eq_tol(config.receivers[0].channels[1].rf_reference_power_dbm, -55.0, 0.000001);
    ck_assert(config.receivers[0].channels[0].stream_enabled);
    ck_assert(config.receivers[0].channels[1].stream_enabled);
    ck_assert_double_eq_tol(config.receivers[0].output_scale, 1.0, 0.000001);
    ck_assert_double_eq_tol(config.receivers[0].channels[1].output_scale, 1.0, 0.000001);
}
END_TEST

START_TEST(defaults_profiles_and_stream_block_samples)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].profile_count = 0; /* omitted -> built-in table */

    char error[128];
    ck_assert_msg(config_validate(&config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.stream_block_samples, SIM_DEFAULT_STREAM_BLOCK_SAMPLES);
    ck_assert_uint_eq(config.receivers[0].profile_count, 6);
    ck_assert_uint_eq(config.receivers[0].channels[0].sample_rate_hz, SIM_RECEIVER_SAMPLE_RATE_HZ);
    ck_assert_uint_eq(config.receivers[0].channels[1].sample_rate_hz, SIM_DDC_SAMPLE_RATE_HZ);
}
END_TEST

START_TEST(channel_sample_rate_follows_profile)
{
    /* The channel selects a profile via bandwidth; the sample rate is overwritten with the
     * profile partner even if the struct carried something else. */
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].channels[1].sample_rate_hz = 12345U;

    char error[128];
    ck_assert_msg(config_validate(&config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.receivers[0].channels[1].sample_rate_hz, SIM_DDC_SAMPLE_RATE_HZ);
}
END_TEST

START_TEST(rejects_unsupported_channel_bandwidth)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].channels[1].bandwidth_hz = 3000000U; /* not in the profile table */

    char error[128];
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "unsupported_channel_bandwidth");

    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].channels[1].bandwidth_hz = 0;
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "channel_bandwidth_required");
}
END_TEST

START_TEST(rejects_bandwidth_exceeding_frontend)
{
    receiver_config_t receiver = valid_receiver(0, 8100, 50000);
    receiver.frontend_bandwidth_hz = 20000000ULL;
    /* channel 0 selects the 80 MHz profile, wider than the 20 MHz front end */
    char error[128];
    ck_assert(!receiver_validate(&receiver, error, sizeof(error)));
    ck_assert_str_eq(error, "bandwidth_exceeds_frontend");
}
END_TEST

START_TEST(rejects_invalid_profiles)
{
    receiver_config_t receiver = valid_receiver(0, 8100, 50000);
    receiver.profiles[2].sample_rate_hz = receiver.profiles[2].bandwidth_hz - 1U; /* rate < bandwidth */
    char error[128];
    ck_assert(!receiver_validate(&receiver, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_profile");

    receiver = valid_receiver(0, 8100, 50000);
    receiver.profiles[1].bandwidth_hz = receiver.profiles[0].bandwidth_hz;
    ck_assert(!receiver_validate(&receiver, error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_profile_bandwidth");

    receiver = valid_receiver(0, 8100, 50000);
    receiver.profile_count = 0;
    ck_assert(!receiver_validate(&receiver, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_profiles");
}
END_TEST

START_TEST(parses_receiver_profiles_from_yaml)
{
    const char *path = write_temp_config_body(
        "",
        "    profiles:\n"
        "      - { bandwidth_hz: 80000000, sample_rate_hz: 98304000, name: \"80M\" }\n"
        "      - { bandwidth_hz: 5000000, sample_rate_hz: 6144000, name: \"5M\" }\n"
        "    channels:\n"
        "      - { channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: 50000 }\n"
        "      - { channel_id: 1, center_frequency_hz: 10000000000, bandwidth_hz: 5000000, udp_output_port: 50001 }\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.receivers[0].profile_count, 2);
    ck_assert_str_eq(config.receivers[0].profiles[1].name, "5M");
    ck_assert_uint_eq(config.receivers[0].channel_count, 2);
    ck_assert_uint_eq(config.receivers[0].channels[1].bandwidth_hz, 5000000U);
    ck_assert_uint_eq(config.receivers[0].channels[1].sample_rate_hz, 6144000U);
}
END_TEST

START_TEST(rejects_channel_bandwidth_outside_profiles_in_yaml)
{
    const char *path = write_temp_config_body(
        "",
        "    profiles:\n"
        "      - { bandwidth_hz: 80000000, sample_rate_hz: 98304000 }\n"
        "    channels:\n"
        "      - { channel_id: 0, track_tuner: true, bandwidth_hz: 20000000, udp_output_port: 50000 }\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "unsupported_channel_bandwidth");
}
END_TEST

START_TEST(parses_class_id_and_counts_up_stream_ids)
{
    const char *path = write_temp_config_body(
        "class_id_oui: 11259375\n"
        "class_id_information_code: 4660\n"
        "class_id_packet_code: 22136\n",
        "    channels:\n"
        "      - { channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, udp_output_port: 50000 }\n"
        "      - { channel_id: 1, center_frequency_hz: 10000000000, bandwidth_hz: 20000000, stream_id: 4242, udp_output_port: 50001 }\n"
        "      - { channel_id: 2, center_frequency_hz: 10001000000, bandwidth_hz: 20000000, udp_output_port: 50002 }\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert(config.class_id_present);
    ck_assert_uint_eq(config.class_id_oui, 0xABCDEFU);
    ck_assert_uint_eq(config.class_id_information_code, 0x1234U);
    ck_assert_uint_eq(config.class_id_packet_code, 0x5678U);
    /* Count-up assignment skips the explicit 4242: channel 0 -> 0, channel 1 -> 4242,
     * channel 2 -> 1. */
    ck_assert_uint_eq(config.receivers[0].channels[0].stream_id, 0U);
    ck_assert_uint_eq(config.receivers[0].channels[1].stream_id, 4242U);
    ck_assert_uint_eq(config.receivers[0].channels[2].stream_id, 1U);
}
END_TEST

START_TEST(rejects_out_of_range_class_id_oui)
{
    const char *path = write_temp_config("class_id_oui: 16777216\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_class_id_oui");
}
END_TEST

START_TEST(rejects_out_of_range_class_id_codes)
{
    const char *info_path = write_temp_config("class_id_information_code: 65536\n");
    ck_assert_ptr_nonnull(info_path);
    simulator_config_t config;
    char error[128];
    ck_assert(!config_load_yaml(info_path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_class_id_information_code");

    const char *packet_path = write_temp_config("class_id_packet_code: 65536\n");
    ck_assert_ptr_nonnull(packet_path);
    ck_assert(!config_load_yaml(packet_path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_class_id_packet_code");
}
END_TEST

START_TEST(rejects_out_of_range_stream_id)
{
    const char *path = write_temp_config_body(
        "",
        "    channels:\n"
        "      - { channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, stream_id: 4294967296, udp_output_port: 50000 }\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_stream_id");
}
END_TEST

START_TEST(rejects_legacy_config_keys)
{
    simulator_config_t config;
    char error[128];

    const char *path = write_temp_config_body("", "    ddc:\n      - ddc_id: 0\n        udp_output_port: 50001\n");
    ck_assert_ptr_nonnull(path);
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "legacy_key_ddc_use_channels");

    path = write_temp_config_body("", "    udp_80mhz_output_port: 50000\n");
    ck_assert_ptr_nonnull(path);
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "legacy_key_udp_80mhz_output_port_use_channels");

    path = write_temp_config_body("", "    bandwidth_hz: 80000000\n");
    ck_assert_ptr_nonnull(path);
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "legacy_key_bandwidth_hz_use_frontend_bandwidth_hz");

    path = write_temp_config_body("", "    sample_rate_hz: 98304000\n");
    ck_assert_ptr_nonnull(path);
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "legacy_key_sample_rate_hz_use_channel_profiles");
}
END_TEST

START_TEST(rejects_channel_sample_rate_key)
{
    const char *path = write_temp_config_body(
        "",
        "    channels:\n"
        "      - { channel_id: 0, track_tuner: true, bandwidth_hz: 80000000, sample_rate_hz: 98304000, udp_output_port: 50000 }\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert(!config_load_yaml(path, &config, error, sizeof(error)));
    ck_assert_str_eq(error, "channel_sample_rate_comes_from_profile");
}
END_TEST

START_TEST(parses_stream_cpus_range)
{
    const char *path = write_temp_config("stream_cpus: \"2-5\"\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.stream_cpu_count, 4);
    ck_assert_int_eq(config.stream_cpus[0], 2);
    ck_assert_int_eq(config.stream_cpus[3], 5);
}
END_TEST

START_TEST(parses_stream_cpus_list)
{
    const char *path = write_temp_config("stream_cpus: \"0,2,4-6\"\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.stream_cpu_count, 5);
    const int expected[] = {0, 2, 4, 5, 6};
    for (size_t i = 0; i < 5; i++) {
        ck_assert_int_eq(config.stream_cpus[i], expected[i]);
    }
}
END_TEST

START_TEST(legacy_stream_cpu_maps_to_single_element_set)
{
    const char *path = write_temp_config("stream_cpu: 3\n");
    ck_assert_ptr_nonnull(path);
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.stream_cpu_count, 1);
    ck_assert_int_eq(config.stream_cpus[0], 3);
}
END_TEST

START_TEST(rejects_invalid_stream_block_samples)
{
    simulator_config_t config = {
        .schema_version = 1,
        .stream_block_samples = SIM_MAX_STREAM_BLOCK_SAMPLES + 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);

    char error[128];
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_stream_block_samples");
}
END_TEST

START_TEST(rejects_invalid_stream_cpu)
{
    simulator_config_t config = {
        .schema_version = 1,
        .stream_cpu = -2,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);

    char error[128];
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_stream_cpu");
}
END_TEST

START_TEST(rejects_zero_scan_rate_only_in_scan_mode)
{
    /* Scan mode (span > front-end bandwidth) requires a positive scan rate. */
    receiver_config_t scan = valid_receiver(0, 8100, 50000);
    scan.frequency_start_hz = 9960000000ULL;
    scan.frequency_stop_hz = 10060000000ULL; /* 100 MHz span > 80 MHz front end -> scan */
    scan.scan_rate_hz_per_s = 0.0;
    char error[128];
    ck_assert(!receiver_validate(&scan, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_scan_rate");

    /* Fixed mode (span <= front-end bandwidth) ignores the scan rate. */
    receiver_config_t fixed = valid_receiver(0, 8100, 50000);
    fixed.frequency_start_hz = 9960000000ULL;
    fixed.frequency_stop_hz = 10040000000ULL; /* 80 MHz span == front end -> fixed */
    fixed.scan_rate_hz_per_s = 0.0;
    ck_assert_msg(receiver_validate(&fixed, error, sizeof(error)), "%s", error);
}
END_TEST

START_TEST(rejects_duplicate_udp_port_within_receiver)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].channels[1].udp_output.port = 50000;

    char error[128];
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_udp_port");
}
END_TEST

START_TEST(rejects_duplicate_ports_across_receivers)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 2,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[1] = valid_receiver(1, 8110, 50004);

    char error[128];
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_udp_port");

    config.receivers[1] = valid_receiver(1, 8100, 50100);
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_rest_port");
}
END_TEST

START_TEST(rejects_invalid_output_scale)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].output_scale = -1.0;

    char error[128];
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_output_scale");

    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].channels[2].output_scale = -0.5;
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_channel");
}
END_TEST

START_TEST(channel_in_window_logic)
{
    receiver_config_t receiver = valid_receiver(0, 8100, 50000);
    /* Fixed tuner centred at 10 GHz, 80 MHz front end. */
    ck_assert(receiver_channel_in_window(&receiver, &receiver.channels[0], 0));

    /* Fixed channel fully inside the window. */
    receiver.channels[1].center_frequency_hz = 10005000000ULL;
    ck_assert(receiver_channel_in_window(&receiver, &receiver.channels[1], 0));

    /* Fixed channel straddling the window edge (centre at the +40 MHz edge). */
    receiver.channels[1].center_frequency_hz = 10040000000ULL;
    ck_assert(!receiver_channel_in_window(&receiver, &receiver.channels[1], 0));

    /* Tuner-tracking channel wider than the front end is never in window. */
    receiver.channels[0].bandwidth_hz = 100000000U;
    ck_assert(!receiver_channel_in_window(&receiver, &receiver.channels[0], 0));
}
END_TEST

START_TEST(ddc_cache_budget_defaults_to_2gib)
{
    simulator_config_t config;
    char error[128];
    /* Key absent -> 2 GiB default. */
    const char *path = write_temp_config("");
    ck_assert_ptr_nonnull(path);
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.ddc_cache_max_bytes, 2ULL << 30);
    /* Explicit 0 disables the intermediate cache (direct cascade every block). */
    path = write_temp_config_body("ddc_cache_max_bytes: 0\n", default_channels_yaml());
    ck_assert_ptr_nonnull(path);
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.ddc_cache_max_bytes, 0);
    /* Explicit values pass through. */
    path = write_temp_config_body("ddc_cache_max_bytes: 536870912\n", default_channels_yaml());
    ck_assert_ptr_nonnull(path);
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.ddc_cache_max_bytes, 536870912);
}
END_TEST

START_TEST(asset_cache_budget_defaults_to_16gib)
{
    simulator_config_t config;
    char error[128];
    /* Key absent -> 16 GiB default. */
    const char *path = write_temp_config("");
    ck_assert_ptr_nonnull(path);
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.asset_cache_max_bytes, 16ULL << 30);
    /* Explicit 0 keeps the legacy unlimited-RAM meaning. */
    path = write_temp_config_body("asset_cache_max_bytes: 0\n", default_channels_yaml());
    ck_assert_ptr_nonnull(path);
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.asset_cache_max_bytes, 0);
    /* Explicit values pass through. */
    path = write_temp_config_body("asset_cache_max_bytes: 1048576\n", default_channels_yaml());
    ck_assert_ptr_nonnull(path);
    ck_assert_msg(config_load_yaml(path, &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.asset_cache_max_bytes, 1048576);
}
END_TEST

Suite *config_suite(void)
{
    Suite *suite = suite_create("config");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, asset_cache_budget_defaults_to_16gib);
    tcase_add_test(tc, ddc_cache_budget_defaults_to_2gib);
    tcase_add_test(tc, loads_instance_config);
    tcase_add_test(tc, defaults_profiles_and_stream_block_samples);
    tcase_add_test(tc, channel_sample_rate_follows_profile);
    tcase_add_test(tc, rejects_unsupported_channel_bandwidth);
    tcase_add_test(tc, rejects_bandwidth_exceeding_frontend);
    tcase_add_test(tc, rejects_invalid_profiles);
    tcase_add_test(tc, parses_receiver_profiles_from_yaml);
    tcase_add_test(tc, rejects_channel_bandwidth_outside_profiles_in_yaml);
    tcase_add_test(tc, parses_class_id_and_counts_up_stream_ids);
    tcase_add_test(tc, rejects_out_of_range_class_id_oui);
    tcase_add_test(tc, rejects_out_of_range_class_id_codes);
    tcase_add_test(tc, rejects_out_of_range_stream_id);
    tcase_add_test(tc, rejects_legacy_config_keys);
    tcase_add_test(tc, rejects_channel_sample_rate_key);
    tcase_add_test(tc, parses_stream_cpus_range);
    tcase_add_test(tc, parses_stream_cpus_list);
    tcase_add_test(tc, legacy_stream_cpu_maps_to_single_element_set);
    tcase_add_test(tc, rejects_invalid_stream_block_samples);
    tcase_add_test(tc, rejects_invalid_stream_cpu);
    tcase_add_test(tc, rejects_zero_scan_rate_only_in_scan_mode);
    tcase_add_test(tc, rejects_duplicate_udp_port_within_receiver);
    tcase_add_test(tc, rejects_duplicate_ports_across_receivers);
    tcase_add_test(tc, rejects_invalid_output_scale);
    tcase_add_test(tc, channel_in_window_logic);
    suite_add_tcase(suite, tc);
    return suite;
}
