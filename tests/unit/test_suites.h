#ifndef TEST_SUITES_H
#define TEST_SUITES_H

#include <check.h>

Suite *config_suite(void);
Suite *scenario_suite(void);
Suite *timebase_suite(void);
Suite *iq_file_reader_suite(void);
Suite *renderer_suite(void);
Suite *ringbuffer_suite(void);

#endif
