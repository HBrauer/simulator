#include "scenario.h"
#include "test_suites.h"

#include <check.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_le16(FILE *file, uint16_t value)
{
    fputc((int)(value & 0xffU), file);
    fputc((int)((value >> 8U) & 0xffU), file);
}

static void write_le32(FILE *file, uint32_t value)
{
    fputc((int)(value & 0xffU), file);
    fputc((int)((value >> 8U) & 0xffU), file);
    fputc((int)((value >> 16U) & 0xffU), file);
    fputc((int)((value >> 24U) & 0xffU), file);
}

static void write_test_wav(const char *path)
{
    FILE *file = fopen(path, "wb");
    ck_assert_ptr_nonnull(file);
    const uint16_t channels = 1;
    const uint32_t sample_rate = 48000;
    const uint16_t bits_per_sample = 16;
    const uint32_t frames = 8;
    const uint32_t data_bytes = frames * channels * (bits_per_sample / 8U);

    fwrite("RIFF", 1U, 4U, file);
    write_le32(file, 36U + data_bytes);
    fwrite("WAVE", 1U, 4U, file);
    fwrite("fmt ", 1U, 4U, file);
    write_le32(file, 16U);
    write_le16(file, 1U);
    write_le16(file, channels);
    write_le32(file, sample_rate);
    write_le32(file, sample_rate * channels * (bits_per_sample / 8U));
    write_le16(file, (uint16_t)(channels * (bits_per_sample / 8U)));
    write_le16(file, bits_per_sample);
    fwrite("data", 1U, 4U, file);
    write_le32(file, data_bytes);
    for (uint32_t i = 0; i < frames; i++) {
        write_le16(file, (uint16_t)(int16_t)(1000 + (int32_t)i * 100));
    }
    fclose(file);
}

START_TEST(loads_and_validates_scenario)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_str_eq(scenario.scenario_id, "test_scenario_001");
    ck_assert_uint_eq(scenario.source_count, 1);
    ck_assert_uint_eq(scenario.signal_count, 1);
}
END_TEST

START_TEST(loads_optional_noise_floor)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/burst_1s_every_5s.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert(scenario.noise_floor.enabled);
    ck_assert(scenario.noise_floor.use_density);
    ck_assert_double_eq_tol(scenario.noise_floor.power_dbm_per_hz, -160.0, 0.001);
    ck_assert_uint_eq(scenario.noise_floor.seed, 49152);
}
END_TEST

START_TEST(loads_audio_wav_modulation_scenario)
{
    char wav_path[] = "/tmp/sdr_audio_source_XXXXXX";
    int wav_fd = mkstemp(wav_path);
    ck_assert_int_ge(wav_fd, 0);
    close(wav_fd);
    write_test_wav(wav_path);

    char json_path[] = "/tmp/sdr_audio_scenario_XXXXXX";
    int json_fd = mkstemp(json_path);
    ck_assert_int_ge(json_fd, 0);
    FILE *json = fdopen(json_fd, "w");
    ck_assert_ptr_nonnull(json);
    fprintf(json,
            "{"
            "\"schema_version\":1,"
            "\"scenario_id\":\"audio_mods\","
            "\"sources\":[{"
            "\"id\":\"audio_001\","
            "\"source_type\":\"audio_file\","
            "\"file\":\"%s\","
            "\"format\":\"wav\","
            "\"sample_rate_hz\":48000,"
            "\"bandwidth_hz\":200000,"
            "\"center_frequency_hz\":0,"
            "\"nominal_level_dbfs\":-6.0"
            "}],"
            "\"signals\":["
            "{\"signal_id\":\"fm\",\"source_reference\":\"audio_001\",\"modulation\":\"wbfm\",\"center_frequency_hz\":10000000000,\"bandwidth_hz\":200000,\"power_dbm\":-60.0,\"fm_deviation_hz\":75000,\"start_time_s\":0.0,\"repeat_interval_s\":1.0},"
            "{\"signal_id\":\"am\",\"source_reference\":\"audio_001\",\"modulation\":\"am\",\"center_frequency_hz\":10001000000,\"bandwidth_hz\":10000,\"power_dbm\":-60.0,\"am_depth\":0.7,\"start_time_s\":0.0,\"repeat_interval_s\":1.0},"
            "{\"signal_id\":\"usb\",\"source_reference\":\"audio_001\",\"modulation\":\"usb\",\"center_frequency_hz\":10002000000,\"bandwidth_hz\":3000,\"power_dbm\":-60.0,\"start_time_s\":0.0,\"repeat_interval_s\":1.0},"
            "{\"signal_id\":\"lsb\",\"source_reference\":\"audio_001\",\"modulation\":\"lsb\",\"center_frequency_hz\":10003000000,\"bandwidth_hz\":3000,\"power_dbm\":-60.0,\"start_time_s\":0.0,\"repeat_interval_s\":1.0}"
            "]"
            "}",
            wav_path);
    fclose(json);

    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json(json_path, &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.source_count, 1);
    ck_assert_uint_eq(scenario.sources[0].source_kind, SCENARIO_SOURCE_AUDIO_FILE);
    ck_assert_uint_eq(scenario.sources[0].sample_count, 8);
    ck_assert_uint_eq(scenario.signal_count, 4);
    ck_assert_uint_eq(scenario.signals[0].modulation, SCENARIO_MODULATION_WBFM);
    ck_assert_uint_eq(scenario.signals[1].modulation, SCENARIO_MODULATION_AM);
    ck_assert_uint_eq(scenario.signals[2].modulation, SCENARIO_MODULATION_USB);
    ck_assert_uint_eq(scenario.signals[3].modulation, SCENARIO_MODULATION_LSB);

    unlink(json_path);
    unlink(wav_path);
}
END_TEST

START_TEST(derives_source_sample_count_from_file_size)
{
    char path[] = "/tmp/sdr_scenario_without_sample_count_XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    FILE *file = fdopen(fd, "w");
    ck_assert_ptr_nonnull(file);
    fprintf(file,
        "{"
        "\"schema_version\":1,"
        "\"scenario_id\":\"derive_sample_count\","
        "\"sources\":[{"
        "\"id\":\"asset_fsk_001\","
        "\"source_type\":\"iq_file\","
        "\"file\":\"simulator/assets/fsk_20mhz.c16\","
        "\"format\":\"ci16\","
        "\"byte_order\":\"little_endian\","
        "\"iq_layout\":\"interleaved_iq\","
        "\"sample_rate_hz\":24576000,"
        "\"bandwidth_hz\":20000000,"
        "\"center_frequency_hz\":0,"
        "\"nominal_level_dbfs\":-12.0"
        "}],"
        "\"signals\":[{"
        "\"signal_id\":\"sig_fsk_asset\","
        "\"source_reference\":\"asset_fsk_001\","
        "\"center_frequency_hz\":10005000000,"
        "\"bandwidth_hz\":20000000,"
        "\"power_dbm\":-55.0,"
        "\"start_time_s\":0.0,"
        "\"repeat_interval_s\":1.0"
        "}]"
        "}");
    fclose(file);

    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json(path, &scenario, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.sources[0].sample_count, 0);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.sources[0].sample_count, 24576);
    unlink(path);
}
END_TEST

START_TEST(rejects_duplicate_source_ids)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.source_count = 2;
    scenario.sources[1] = scenario.sources[0];
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_source_id");
}
END_TEST

START_TEST(rejects_duplicate_signal_ids)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.signal_count = 2;
    scenario.signals[1] = scenario.signals[0];
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "duplicate_signal_id");
}
END_TEST

START_TEST(rejects_missing_source_reference)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    snprintf(scenario.signals[0].source_reference, sizeof(scenario.signals[0].source_reference), "%s", "does_not_exist");
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "missing_source_reference");
}
END_TEST

START_TEST(resolves_relative_asset_path_against_base_dir)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    /* Path relative to base_dir "simulator" resolves to simulator/assets/fsk_20mhz.c16. */
    snprintf(scenario.sources[0].file, sizeof(scenario.sources[0].file), "%s", "assets/fsk_20mhz.c16");
    scenario.sources[0].sample_count = 0;
    ck_assert_msg(scenario_validate(&scenario, "simulator", error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.sources[0].sample_count, 24576);
    ck_assert_str_eq(scenario.sources[0].file, "simulator/assets/fsk_20mhz.c16");
}
END_TEST

START_TEST(loads_replay_range_shift_scenario)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/replay_range_shift.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.signal_count, 2);
    ck_assert_uint_eq(scenario.signals[0].replay_mode, SCENARIO_REPLAY_RANGE);
    ck_assert_uint_eq(scenario.signals[0].replay_range_start_hz, 99900000ULL);
    ck_assert_uint_eq(scenario.signals[0].replay_range_stop_hz, 100100000ULL);
    ck_assert(scenario.signals[0].loop); /* defaults to true for range/shift */
    ck_assert_uint_eq(scenario.signals[1].replay_mode, SCENARIO_REPLAY_SHIFT);
    ck_assert_uint_eq(scenario.signals[1].center_frequency_hz, 100000000ULL);
    ck_assert(scenario.signals[1].loop);
    ck_assert(!scenario.signals[0].passthrough); /* absent in JSON -> defaults false */
}
END_TEST

START_TEST(defaults_to_fixed_replay_without_new_fields)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.signals[0].replay_mode, SCENARIO_REPLAY_FIXED);
    ck_assert(!scenario.signals[0].loop);
    ck_assert_uint_eq(scenario.signals[0].replay_range_start_hz, 0);
}
END_TEST

START_TEST(rejects_invalid_replay_configs)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/replay_range_shift.json", &scenario, error, sizeof(error)), "%s", error);

    scenario_t broken = scenario;
    broken.signals[0].replay_range_start_hz = broken.signals[0].replay_range_stop_hz;
    ck_assert(!scenario_validate(&broken, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "replay_range_invalid");

    broken = scenario;
    broken.signals[1].center_frequency_hz = 0;
    ck_assert(!scenario_validate(&broken, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "replay_shift_missing_center");

    broken = scenario;
    broken.signals[0].modulation = SCENARIO_MODULATION_WBFM;
    ck_assert(!scenario_validate(&broken, ".", error, sizeof(error)));
    /* Non-IQ modulation trips the generic source/modulation check or the replay one; either
     * way range replay on a non-IQ signal must not validate. */
    ck_assert(strcmp(error, "replay_mode_source_mismatch") == 0 ||
              strcmp(error, "signal_modulation_source_mismatch") == 0);
}
END_TEST

START_TEST(rejects_loop_repeat_conflict_and_bad_mode)
{
    char path[] = "/tmp/sdr_scenario_replay_conflict_XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    FILE *file = fdopen(fd, "w");
    ck_assert_ptr_nonnull(file);
    fprintf(file,
        "{"
        "\"schema_version\":1,"
        "\"scenario_id\":\"replay_conflict\","
        "\"sources\":[{"
        "\"id\":\"asset_fsk_001\",\"source_type\":\"iq_file\",\"file\":\"simulator/assets/fsk_20mhz.c16\","
        "\"format\":\"ci16\",\"byte_order\":\"little_endian\",\"iq_layout\":\"interleaved_iq\","
        "\"sample_rate_hz\":24576000,\"bandwidth_hz\":20000000,\"center_frequency_hz\":0,\"nominal_level_dbfs\":-12.0"
        "}],"
        "\"signals\":[{"
        "\"signal_id\":\"sig\",\"source_reference\":\"asset_fsk_001\","
        "\"replay_mode\":\"range\",\"frequency_range\":{\"start_hz\":99900000,\"stop_hz\":100100000},"
        "\"bandwidth_hz\":1000000,\"power_dbm\":-55.0,"
        "\"repeat_interval_s\":1.0"
        "}]"
        "}");
    fclose(file);
    scenario_t scenario;
    char error[128];
    /* range mode implies loop, and loop cannot be combined with a repeat interval */
    ck_assert(!scenario_load_json(path, &scenario, error, sizeof(error)));
    ck_assert_str_eq(error, "loop_repeat_conflict");
    unlink(path);

    char bad_mode_path[] = "/tmp/sdr_scenario_replay_badmode_XXXXXX";
    fd = mkstemp(bad_mode_path);
    ck_assert_int_ge(fd, 0);
    file = fdopen(fd, "w");
    ck_assert_ptr_nonnull(file);
    fprintf(file,
        "{"
        "\"schema_version\":1,"
        "\"scenario_id\":\"replay_badmode\",\"sources\":[],"
        "\"signals\":[{"
        "\"signal_id\":\"sig\",\"source_reference\":\"x\",\"replay_mode\":\"follow\","
        "\"bandwidth_hz\":1000000,\"power_dbm\":-55.0"
        "}]"
        "}");
    fclose(file);
    ck_assert(!scenario_load_json(bad_mode_path, &scenario, error, sizeof(error)));
    ck_assert_str_eq(error, "signal_replay_mode_invalid");
    unlink(bad_mode_path);
}
END_TEST

START_TEST(rejects_passthrough_without_range_or_shift)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/test_scenario_001.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.signals[0].passthrough = true; /* fixed replay mode by default */
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "passthrough_requires_replay_mode");
}
END_TEST

START_TEST(rejects_passthrough_without_loop)
{
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/replay_range_shift.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.signals[0].passthrough = true;
    scenario.signals[0].loop = false;
    scenario.signals[0].repeat_interval_s = 1.0;
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "passthrough_requires_loop");
}
END_TEST

START_TEST(loads_passthrough_variant_scenario)
{
    /* The passthrough demo carries a variant source (one capture per rate) referenced by a
     * passthrough signal; it must parse and validate, with variant rates/counts populated. */
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/replay_passthrough_demo.json", &scenario, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(scenario.sources[0].passthrough_variant_count, 2);
    ck_assert_uint_eq(scenario.sources[0].passthrough_variants[0].sample_rate_hz, 1536000);
    ck_assert_uint_eq(scenario.sources[0].passthrough_variants[1].sample_rate_hz, 24576000);
    ck_assert(scenario.signals[0].passthrough);
    ck_assert_msg(scenario_validate(&scenario, ".", error, sizeof(error)), "%s", error);
    /* Validation fills each variant's sample_count from its file. */
    ck_assert_uint_gt(scenario.sources[0].passthrough_variants[0].sample_count, 0);
    ck_assert_uint_gt(scenario.sources[0].passthrough_variants[1].sample_count, 0);
}
END_TEST

START_TEST(rejects_passthrough_without_variants)
{
    /* A passthrough signal whose source has no variants is a misconfiguration: passthrough only
     * streams matched-rate captures, so it needs a variant set. */
    scenario_t scenario;
    char error[128];
    ck_assert_msg(scenario_load_json("simulator/scenarios/replay_range_shift.json", &scenario, error, sizeof(error)), "%s", error);
    scenario.signals[0].passthrough = true; /* range mode, loop true, but source has no variants */
    ck_assert(!scenario_validate(&scenario, ".", error, sizeof(error)));
    ck_assert_str_eq(error, "passthrough_requires_variants");
}
END_TEST

Suite *scenario_suite(void)
{
    Suite *suite = suite_create("scenario");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_replay_range_shift_scenario);
    tcase_add_test(tc, rejects_passthrough_without_range_or_shift);
    tcase_add_test(tc, rejects_passthrough_without_loop);
    tcase_add_test(tc, loads_passthrough_variant_scenario);
    tcase_add_test(tc, rejects_passthrough_without_variants);
    tcase_add_test(tc, defaults_to_fixed_replay_without_new_fields);
    tcase_add_test(tc, rejects_invalid_replay_configs);
    tcase_add_test(tc, rejects_loop_repeat_conflict_and_bad_mode);
    tcase_add_test(tc, loads_and_validates_scenario);
    tcase_add_test(tc, loads_optional_noise_floor);
    tcase_add_test(tc, loads_audio_wav_modulation_scenario);
    tcase_add_test(tc, derives_source_sample_count_from_file_size);
    tcase_add_test(tc, rejects_duplicate_source_ids);
    tcase_add_test(tc, rejects_duplicate_signal_ids);
    tcase_add_test(tc, rejects_missing_source_reference);
    tcase_add_test(tc, resolves_relative_asset_path_against_base_dir);
    suite_add_tcase(suite, tc);
    return suite;
}
