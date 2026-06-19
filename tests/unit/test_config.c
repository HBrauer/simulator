#include "config.h"
#include "test_suites.h"

#include <check.h>

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
    ck_assert_msg(config_load_yaml("configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.receiver_count, 1);
    ck_assert_uint_eq(config.receivers[0].id, 0);
    ck_assert_uint_eq(config.receivers[0].rest_port, 8100);
    ck_assert_uint_eq(config.receivers[0].ddc[3].udp_output.port, 50004);
    ck_assert_uint_eq(config.stream_block_samples, 1024);
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
    tcase_add_test(tc, rejects_invalid_stream_block_samples);
    tcase_add_test(tc, rejects_duplicate_udp_port_within_receiver);
    tcase_add_test(tc, rejects_duplicate_ports_across_receivers);
    tcase_add_test(tc, rejects_invalid_output_scale);
    suite_add_tcase(suite, tc);
    return suite;
}
