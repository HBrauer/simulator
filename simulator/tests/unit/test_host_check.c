#include "test_suites.h"
#include "host_check.h"
#include "streamer.h"
#include "vita49_packet.h"

#include <check.h>
#include <stdio.h>
#include <string.h>

/* One receiver with `channels` channels at 24.576 MS/s, CI16, one render thread each. */
static void make_config(simulator_config_t *config, size_t channels)
{
    memset(config, 0, sizeof(*config));
    config->receiver_count = 1;
    receiver_config_t *receiver = &config->receivers[0];
    receiver->id = 3;
    snprintf(receiver->udp_output_host, sizeof(receiver->udp_output_host), "192.0.2.1");
    receiver->channel_count = channels;
    for (size_t c = 0; c < channels; c++) {
        channel_config_t *channel = &receiver->channels[c];
        channel->id = (uint32_t)c;
        channel->sample_rate_hz = 24576000U;
        channel->rate_count = 1;
        channel->rates[0].sample_rate_hz = 24576000U;
        channel->render_threads = 1;
        channel->output_format = SIM_OUTPUT_FORMAT_CI16;
    }
}

/* Capture what a check writes so the message can be asserted on. */
typedef struct {
    char text[2048];
    FILE *out;
} capture_t;

static void capture_open(capture_t *capture)
{
    memset(capture->text, 0, sizeof(capture->text));
    capture->out = fmemopen(capture->text, sizeof(capture->text) - 1U, "w");
    ck_assert_ptr_nonnull(capture->out);
}

static const char *capture_close(capture_t *capture)
{
    fclose(capture->out);
    return capture->text;
}

START_TEST(max_datagram_uses_highest_rate_format_and_vrl)
{
    simulator_config_t config;
    make_config(&config, 2);
    config.receivers[0].channels[1].output_format = SIM_OUTPUT_FORMAT_CF32;
    ck_assert_uint_eq(host_check_max_datagram_bytes(&config, &config.receivers[0], 1536),
                      VITA49_IF_DATA_HEADER_BYTES + 1536U * 8U);
    config.class_id_present = true;
    config.receivers[0].vrl_framing = true;
    ck_assert_uint_eq(host_check_max_datagram_bytes(&config, &config.receivers[0], 1536),
                      VITA49_IF_DATA_HEADER_BYTES + VITA49_CLASS_ID_BYTES + 1536U * 8U + VITA49_VRL_OVERHEAD_BYTES);
    /* A low-rate channel shrinks its block below the configured size. */
    make_config(&config, 1);
    config.receivers[0].channels[0].sample_rate_hz = 8000U;
    config.receivers[0].channels[0].rates[0].sample_rate_hz = 8000U;
    ck_assert_uint_eq(host_check_max_datagram_bytes(&config, &config.receivers[0], 1536),
                      VITA49_IF_DATA_HEADER_BYTES + streamer_block_samples_for_rate(1536, 8000U) * 4U);
}
END_TEST

START_TEST(stream_cpu_outside_affinity_is_an_error)
{
    simulator_config_t config;
    make_config(&config, 1);
    config.stream_cpus[0] = 1;
    config.stream_cpus[1] = 7;
    config.stream_cpu_count = 2;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    for (int cpu = 0; cpu < 4; cpu++) {
        CPU_SET((size_t)cpu, &allowed);
    }
    capture_t capture;
    capture_open(&capture);
    host_check_result_t result = {0};
    host_check_stream_cpus(&config, &allowed, capture.out, &result);
    const char *text = capture_close(&capture);
    ck_assert_uint_eq(result.errors, 1U);
    ck_assert_uint_eq(result.warnings, 0U);
    ck_assert_ptr_nonnull(strstr(text, "host check error: stream_cpus lists CPU 7"));
    ck_assert_ptr_nonnull(strstr(text, "CPUs 0-3"));
}
END_TEST

START_TEST(more_stream_threads_than_cpus_warns)
{
    simulator_config_t config;
    make_config(&config, 3); /* 3 x (render + UDP) = 6 threads */
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    for (int cpu = 0; cpu < 8; cpu++) {
        CPU_SET((size_t)cpu, &allowed);
    }
    host_check_result_t result = {0};
    host_check_thread_count(&config, &allowed, NULL, &result);
    ck_assert_uint_eq(result.warnings, 0U);

    /* Pinned to two CPUs: 6 threads on 2 CPUs. */
    config.stream_cpus[0] = 0;
    config.stream_cpus[1] = 1;
    config.stream_cpu_count = 2;
    host_check_thread_count(&config, &allowed, NULL, &result);
    ck_assert_uint_eq(result.warnings, 1U);
}
END_TEST

START_TEST(small_send_buffer_warns_with_sysctl_fix)
{
    simulator_config_t config;
    make_config(&config, 1);
    host_check_result_t result = {0};
    /* 1536 CI16 samples -> ~6 KiB datagrams; a 16-burst needs ~230 KiB. */
    host_check_sndbuf(&config, 1536, 8 * 1024 * 1024, NULL, &result);
    ck_assert_uint_eq(result.warnings, 0U);
    capture_t capture;
    capture_open(&capture);
    host_check_sndbuf(&config, 1536, 64 * 1024, capture.out, &result);
    const char *text = capture_close(&capture);
    ck_assert_uint_eq(result.warnings, 1U);
    ck_assert_ptr_nonnull(strstr(text, "sudo sysctl -w net.core.wmem_max="));
}
END_TEST

START_TEST(datagram_over_mtu_warns)
{
    simulator_config_t config;
    make_config(&config, 1);
    host_check_result_t result = {0};
    host_check_datagram_mtu(&config, &config.receivers[0], 1536, 9000, NULL, &result);
    host_check_datagram_mtu(&config, &config.receivers[0], 1536, 65536, NULL, &result);
    ck_assert_uint_eq(result.warnings, 0U);
    capture_t capture;
    capture_open(&capture);
    host_check_datagram_mtu(&config, &config.receivers[0], 1536, 1500, capture.out, &result);
    const char *text = capture_close(&capture);
    ck_assert_uint_eq(result.warnings, 1U);
    ck_assert_ptr_nonnull(strstr(text, "receiver 3: 6164-byte datagrams exceed the 1500-byte MTU towards 192.0.2.1"));
    ck_assert_ptr_nonnull(strstr(text, "~5 IP fragments"));
}
END_TEST

START_TEST(unsynchronised_clock_warns)
{
    host_check_result_t result = {0};
    host_check_clock(true, NULL, &result);
    ck_assert_uint_eq(result.warnings, 0U);
    host_check_clock(false, NULL, &result);
    ck_assert_uint_eq(result.warnings, 1U);
}
END_TEST

START_TEST(asset_memory_near_available_warns)
{
    host_check_result_t result = {0};
    host_check_memory(700U << 20U, 1000U << 20U, NULL, &result);
    host_check_memory(700U << 20U, 0U, NULL, &result); /* unknown availability: no finding */
    ck_assert_uint_eq(result.warnings, 0U);
    host_check_memory(800U << 20U, 1000U << 20U, NULL, &result);
    ck_assert_uint_eq(result.warnings, 1U);
}
END_TEST

START_TEST(reads_host_memory_facts)
{
    ck_assert_uint_gt(host_check_resident_bytes(), 0U);
    ck_assert_uint_gt(host_check_available_memory_bytes(), 0U);
}
END_TEST

Suite *host_check_suite(void)
{
    Suite *suite = suite_create("host_check");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, max_datagram_uses_highest_rate_format_and_vrl);
    tcase_add_test(tc, stream_cpu_outside_affinity_is_an_error);
    tcase_add_test(tc, more_stream_threads_than_cpus_warns);
    tcase_add_test(tc, small_send_buffer_warns_with_sysctl_fix);
    tcase_add_test(tc, datagram_over_mtu_warns);
    tcase_add_test(tc, unsynchronised_clock_warns);
    tcase_add_test(tc, asset_memory_near_available_warns);
    tcase_add_test(tc, reads_host_memory_facts);
    suite_add_tcase(suite, tc);
    return suite;
}
