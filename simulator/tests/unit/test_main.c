#include "test_suites.h"

#include <stdlib.h>

int main(void)
{
    Suite *suites[] = {
        config_suite(),
        asset_cache_suite(),
        ddc_suite(),
        ddc_cache_suite(),
        scenario_suite(),
        timebase_suite(),
        iq_file_reader_suite(),
        renderer_suite(),
        ringbuffer_suite(),
        streamer_suite(),
        vita49_packet_suite(),
        wav_reader_suite(),
        receiver_c_suite(),
    };
    SRunner *runner = srunner_create(suites[0]);
    for (size_t i = 1; i < sizeof(suites) / sizeof(suites[0]); i++) {
        srunner_add_suite(runner, suites[i]);
    }
    srunner_run_all(runner, CK_NORMAL);
    const int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
