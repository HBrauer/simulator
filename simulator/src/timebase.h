#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool override_enabled;
    uint64_t override_time_ns;
} timebase_t;

void timebase_init(timebase_t *timebase);
void timebase_set_override(timebase_t *timebase, uint64_t scenario_time_ns);
uint64_t timebase_now_ns(const timebase_t *timebase);
double timebase_day_seconds_from_ns(uint64_t scenario_time_ns);

#endif
