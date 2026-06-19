#include "renderer.h"
#include "iq_file_reader.h"
#include "nco.h"
#include "receiver.h"
#include "scenario.h"
#include "timebase.h"

#include <math.h>
#include <string.h>

static bool signal_intersects(uint64_t signal_center, uint32_t signal_bw, uint64_t window_center, uint64_t window_bw)
{
    const int64_t sig_lo = (int64_t)signal_center - (int64_t)(signal_bw / 2U);
    const int64_t sig_hi = (int64_t)signal_center + (int64_t)(signal_bw / 2U);
    const int64_t win_lo = (int64_t)window_center - (int64_t)(window_bw / 2ULL);
    const int64_t win_hi = (int64_t)window_center + (int64_t)(window_bw / 2ULL);
    return sig_hi >= win_lo && sig_lo <= win_hi;
}

bool renderer_render_80mhz_block(const scenario_t *scenario, const receiver_config_t *receiver, uint64_t scenario_time_ns, iq_ci16_t *out, size_t count, render_stats_t *stats)
{
    memset(out, 0, count * sizeof(*out));
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    const uint64_t center_hz = receiver_center_frequency_hz(receiver, scenario_time_ns);
    const double day_s = timebase_day_seconds_from_ns(scenario_time_ns);

    iq_ci16_t scratch[4096];
    const size_t chunk = count < 4096 ? count : 4096;

    for (size_t s = 0; s < scenario->signal_count; s++) {
        const scenario_signal_t *signal = &scenario->signals[s];
        const scenario_source_t *source = scenario_find_source(scenario, signal->source_reference);
        uint64_t sample_offset = 0;
        if (source == NULL || !signal_intersects(signal->center_frequency_hz, signal->bandwidth_hz, center_hz, SIM_RECEIVER_BANDWIDTH_HZ) ||
            !iq_signal_active(signal, source, day_s, &sample_offset)) {
            continue;
        }
        iq_file_reader_t reader;
        char error[128];
        if (!iq_file_reader_open(&reader, source->file, source->sample_count, error, sizeof(error))) {
            continue;
        }
        size_t read_count = 0;
        (void)iq_file_reader_read(&reader, sample_offset, scratch, chunk, &read_count);
        iq_file_reader_close(&reader);

        const double offset_hz = (double)((int64_t)signal->center_frequency_hz - (int64_t)center_hz);
        nco_t nco;
        nco_init(&nco, offset_hz, (double)source->sample_rate_hz);
        iq_ci16_t shifted[4096];
        nco_mix_ci16(&nco, scratch, shifted, read_count, 1.0);
        for (size_t i = 0; i < read_count; i++) {
            out[i].i = sim_clip_i16((double)out[i].i + (double)shifted[i].i);
            out[i].q = sim_clip_i16((double)out[i].q + (double)shifted[i].q);
        }
        if (stats != NULL) {
            stats->active_signals++;
        }
    }
    if (stats != NULL) {
        stats->samples_rendered = count;
    }
    return true;
}
