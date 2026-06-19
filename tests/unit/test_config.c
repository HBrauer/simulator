#include "config.h"
#include "test_suites.h"

#include <check.h>

START_TEST(loads_instance_config)
{
    simulator_config_t config;
    char error[128];
    ck_assert_msg(config_load_yaml("configs/instance_001.yaml", &config, error, sizeof(error)), "%s", error);
    ck_assert_uint_eq(config.receiver_count, 1);
    ck_assert_uint_eq(config.receivers[0].id, 0);
    ck_assert_uint_eq(config.receivers[0].rest_port, 8100);
    ck_assert_uint_eq(config.receivers[0].ddc[3].udp_output.port, 50004);
}
END_TEST

Suite *config_suite(void)
{
    Suite *suite = suite_create("config");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, loads_instance_config);
    suite_add_tcase(suite, tc);
    return suite;
}
