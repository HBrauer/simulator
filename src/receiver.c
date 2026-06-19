#include "receiver.h"

#include <math.h>
#include <stdio.h>

receiver_mode_t receiver_effective_mode(const receiver_config_t *receiver)
{
    return (receiver->frequency_stop_hz - receiver->frequency_start_hz <= SIM_RECEIVER_BANDWIDTH_HZ)
        ? RECEIVER_MODE_FIXED
        : RECEIVER_MODE_SCAN;
}

uint64_t receiver_fixed_center_hz(const receiver_config_t *receiver)
{
    return receiver->frequency_start_hz / 2ULL + receiver->frequency_stop_hz / 2ULL +
        ((receiver->frequency_start_hz & 1ULL) && (receiver->frequency_stop_hz & 1ULL) ? 1ULL : 0ULL);
}

uint64_t receiver_center_frequency_hz(const receiver_config_t *receiver, uint64_t scenario_time_ns)
{
    if (receiver_effective_mode(receiver) == RECEIVER_MODE_FIXED) {
        return receiver_fixed_center_hz(receiver);
    }

    const double span = (double)(receiver->frequency_stop_hz - receiver->frequency_start_hz);
    const double t = (double)scenario_time_ns / 1000000000.0;
    const double scan_rate = receiver->scan_rate_hz_per_s > 0.0 ? receiver->scan_rate_hz_per_s : 100000000000.0;
    const double position = fmod(scan_rate * t, span);
    return receiver->frequency_start_hz + (uint64_t)llround(position);
}

bool receiver_ddc_in_window(const receiver_config_t *receiver, const ddc_config_t *ddc, uint64_t scenario_time_ns)
{
    const uint64_t receiver_center = receiver_center_frequency_hz(receiver, scenario_time_ns);
    const int64_t receiver_low = (int64_t)receiver_center - (int64_t)(SIM_RECEIVER_BANDWIDTH_HZ / 2ULL);
    const int64_t receiver_high = (int64_t)receiver_center + (int64_t)(SIM_RECEIVER_BANDWIDTH_HZ / 2ULL);
    const int64_t ddc_low = (int64_t)ddc->center_frequency_hz - (int64_t)(ddc->bandwidth_hz / 2U);
    const int64_t ddc_high = (int64_t)ddc->center_frequency_hz + (int64_t)(ddc->bandwidth_hz / 2U);
    return ddc_low >= receiver_low && ddc_high <= receiver_high;
}

bool receiver_validate(const receiver_config_t *receiver, char *error, size_t error_size)
{
    if (receiver->frequency_start_hz > SIM_MAX_RF_HZ || receiver->frequency_stop_hz > SIM_MAX_RF_HZ ||
        receiver->frequency_stop_hz <= receiver->frequency_start_hz) {
        snprintf(error, error_size, "invalid_frequency");
        return false;
    }
    if (receiver->rest_port == 0 || receiver->udp_80mhz_output.port == 0) {
        snprintf(error, error_size, "invalid_port");
        return false;
    }
    if (receiver->output_scale <= 0.0) {
        snprintf(error, error_size, "invalid_output_scale");
        return false;
    }
    for (size_t i = 0; i < SIM_DDC_COUNT; i++) {
        if (receiver->ddc[i].id != i || receiver->ddc[i].udp_output.port == 0 ||
            receiver->ddc[i].center_frequency_hz > SIM_MAX_RF_HZ || receiver->ddc[i].output_scale <= 0.0) {
            snprintf(error, error_size, "invalid_ddc");
            return false;
        }
    }
    snprintf(error, error_size, "ok");
    return true;
}
