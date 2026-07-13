#ifndef TEST_SUITES_H
#define TEST_SUITES_H

#include <check.h>

Suite *config_suite(void);
Suite *asset_cache_suite(void);
Suite *ddc_suite(void);
Suite *scenario_suite(void);
Suite *timebase_suite(void);
Suite *iq_file_reader_suite(void);
Suite *renderer_suite(void);
Suite *ringbuffer_suite(void);
Suite *streamer_suite(void);
Suite *vita49_packet_suite(void);
Suite *wav_reader_suite(void);
Suite *receiver_c_suite(void);

#endif
