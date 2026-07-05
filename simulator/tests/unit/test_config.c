#include "config.h"
#include "receiver.h"
#include "test_suites.h"

#include <check.h>
#include <stdio.h>
#include <unistd.h>

static const char *write_temp_config(const char *body)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/sim_cfg_%d.yaml", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NULL;
    }
    fputs("schema_version: 1\n", f);
    fputs("instance_id: \"t\"\n", f);
    fputs("scenario_file: \"simulator/scenarios/test_scenario_001.json\"\n", f);
    fputs(body, f);
    fputs("receivers:\n"
          "  - receiver_id: 0\n"
          "    rest_bind_host: \"127.0.0.1\"\n"
          "    rest_port: 8100\n"
          "    udp_output_host: \"127.0.0.1\"\n"
          "    frequency_start_hz: 9960000000\n"
          "    frequency_stop_hz: 10040000000\n"
          "    udp_80mhz_output_port: 50000\n"
          "    ddc:\n"
          "      - ddc_id: 0\n        center_frequency_hz: 10000000000\n        udp_output_port: 50001\n"
          "      - ddc_id: 1\n        center_frequency_hz: 10001000000\n        udp_output_port: 50002\n"
          "      - ddc_id: 2\n        center_frequency_hz: 10002000000\n        udp_output_port: 50003\n"
          "      - ddc_id: 3\n        center_frequency_hz: 10003000000\n        udp_output_port: 50004\n",
          f);
    fclose(f);
    return path;
}

static receiver_config_t valid_receiver(uint32_t id, uint16_t rest_port, uint16_t udp_base)
{
    receiver_config_t receiver = {
        .id = id,
        .rest_port = rest_port,
        .frequency_start_hz = 9960000000ULL,
        .frequency_stop_hz = 10040000000ULL,
        .bandwidth_hz = SIM_RECEIVER_BANDWIDTH_HZ,
        .sample_rate_hz = SIM_RECEIVER_SAMPLE_RATE_HZ,
        .scan_rate_hz_per_s = 100000000000.0,
        .output_scale = 1.0,
        .rf_reference_power_dbm = -55.0,
        .stream_enabled = true,
        .udp_80mhz_output = {.port = udp_base},
    };
    for (size_t i = 0; i < SIM_DDC_COUNT; i++) {
        receiver.ddc[i] = (ddc_config_t){
            .id = (uint32_t)i,
            .center_frequency_hz = 10000000000ULL + (uint64_t)i * 1000000ULL,
            .bandwidth_hz = SIM_DDC_BANDWIDTH_HZ,
            .sample_rate_hz = SIM_DDC_SAMPLE_RATE_HZ,
            .output_scale = 1.0,
            .rf_reference_power_dbm = -55.0,
            .stream_enabled = true,
            .udp_output = {.port = (uint16_t)(udp_base + 1 + i)},
        };
    }
    return receiver;
}

START_TEST(loads_instance_config)
{
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml("simulator/configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.receiver_count, 1);
    ck_assert_uint_eq(config.receivers[0].id, 0);
    ck_assert_uint_eq(config.receivers[0].rest_port, 8100);
    ck_assert_uint_eq(config.receivers[0].ddc[3].udp_output.port, 50004);
    ck_assert_uint_eq(config.stream_block_samples, 1024);
    ck_assert_int_eq(config.stream_cpu, -1);
    ck_assert_uint_eq(config.stream_cpu_count, 0); /* absent -> no pinning */
    ck_assert_uint_eq(config.asset_cache_max_bytes, 0);
    ck_assert_uint_eq(config.receivers[0].bandwidth_hz, 80000000ULL);
    ck_assert_uint_eq(config.receivers[0].sample_rate_hz, 98304000U);
    ck_assert_uint_eq(config.receivers[0].ddc[0].bandwidth_hz, 20000000U);
    ck_assert_uint_eq(config.receivers[0].ddc[0].sample_rate_hz, 24576000U);
    ck_assert_double_eq_tol(config.receivers[0].rf_reference_power_dbm, -55.0, 0.000001);
    ck_assert_double_eq_tol(config.receivers[0].ddc[0].rf_reference_power_dbm, -55.0, 0.000001);
    ck_assert(config.receivers[0].stream_enabled);
    ck_assert(config.receivers[0].ddc[0].stream_enabled);
    ck_assert_double_eq_tol(config.receivers[0].output_scale, 1.0, 0.000001);
    ck_assert_double_eq_tol(config.receivers[0].ddc[0].output_scale, 1.0, 0.000001);
}
END_TEST

START_TEST(defaults_stream_block_samples)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);

    char error[128];
    ck_assert_msg(config_validate(&config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.stream_block_samples, SIM_DEFAULT_STREAM_BLOCK_SAMPLES);
    ck_assert_uint_eq(config.receivers[0].bandwidth_hz, SIM_RECEIVER_BANDWIDTH_HZ);
    ck_assert_uint_eq(config.receivers[0].sample_rate_hz, SIM_RECEIVER_SAMPLE_RATE_HZ);
    ck_assert_uint_eq(config.receivers[0].ddc[0].bandwidth_hz, SIM_DDC_BANDWIDTH_HZ);
    ck_assert_uint_eq(config.receivers[0].ddc[0].sample_rate_hz, SIM_DDC_SAMPLE_RATE_HZ);
}
END_TEST

START_TEST(rejects_invalid_sample_rate_or_bandwidth)
{
    simulator_config_t config = {
        .schema_version = 1,
        .receiver_count = 1,
    };
    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].sample_rate_hz = 0;

    char error[128];
    ck_assert(!receiver_validate(&config.receivers[0], error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_sample_rate_or_bandwidth");

    config.receivers[0] = valid_receiver(0, 8100, 50000);
    config.receivers[0].ddc[1].bandwidth_hz = 0;
    ck_assert(!receiver_validate(&config.receivers[0], error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_ddc");
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
    /* Scan mode (span > bandwidth) requires a positive scan rate. */
    receiver_config_t scan = valid_receiver(0, 8100, 50000);
    scan.frequency_start_hz = 9960000000ULL;
    scan.frequency_stop_hz = 10060000000ULL; /* 100 MHz span > 80 MHz bandwidth -> scan */
    scan.scan_rate_hz_per_s = 0.0;
    char error[128];
    ck_assert(!receiver_validate(&scan, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_scan_rate");

    /* Fixed mode (span <= bandwidth) ignores the scan rate. */
    receiver_config_t fixed = valid_receiver(0, 8100, 50000);
    fixed.frequency_start_hz = 9960000000ULL;
    fixed.frequency_stop_hz = 10040000000ULL; /* 80 MHz span == bandwidth -> fixed */
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
    config.receivers[0].ddc[0].udp_output.port = 50000;

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
    config.receivers[0].ddc[2].output_scale = -0.5;
    ck_assert(!config_validate(&config, error, sizeof(error)));
    ck_assert_str_eq(error, "invalid_ddc");
}
END_TEST

Suite *config_suite(void)
{
    Suite *suite = suite_create("config");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_instance_config);
    tcase_add_test(tc, defaults_stream_block_samples);
    tcase_add_test(tc, rejects_invalid_sample_rate_or_bandwidth);
    tcase_add_test(tc, parses_stream_cpus_range);
    tcase_add_test(tc, parses_stream_cpus_list);
    tcase_add_test(tc, legacy_stream_cpu_maps_to_single_element_set);
    tcase_add_test(tc, rejects_invalid_stream_block_samples);
    tcase_add_test(tc, rejects_invalid_stream_cpu);
    tcase_add_test(tc, rejects_zero_scan_rate_only_in_scan_mode);
    tcase_add_test(tc, rejects_duplicate_udp_port_within_receiver);
    tcase_add_test(tc, rejects_duplicate_ports_across_receivers);
    tcase_add_test(tc, rejects_invalid_output_scale);
    suite_add_tcase(suite, tc);
    return suite;
}
