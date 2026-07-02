#include "timebase.h"

#include <time.h>

void timebase_init(timebase_t *timebase)
{
    timebase->override_enabled = false;
    timebase->override_time_ns = 0;
}

void timebase_set_override(timebase_t *timebase, uint64_t scenario_time_ns)
{
    timebase->override_enabled = true;
    timebase->override_time_ns = scenario_time_ns;
}

uint64_t timebase_now_ns(const timebase_t *timebase)
{
    if (timebase->override_enabled) {
        return timebase->override_time_ns;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t day_ns = 86400ULL * 1000000000ULL;
    const uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return now_ns % day_ns;
}

double timebase_day_seconds_from_ns(uint64_t scenario_time_ns)
{
    const uint64_t day_ns = 86400ULL * 1000000000ULL;
    return (double)(scenario_time_ns % day_ns) / 1000000000.0;
}
