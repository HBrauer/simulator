#include "receiver.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

receiver_mode_t receiver_effective_mode(const receiver_config_t *receiver)
{
    return (receiver->frequency_max_hz - receiver->frequency_min_hz <= receiver->bandwidth_hz)
        ? RECEIVER_MODE_FIXED
        : RECEIVER_MODE_SCAN;
}

uint64_t receiver_fixed_center_hz(const receiver_config_t *receiver)
{
    return receiver->frequency_min_hz / 2ULL + receiver->frequency_max_hz / 2ULL +
        ((receiver->frequency_min_hz & 1ULL) && (receiver->frequency_max_hz & 1ULL) ? 1ULL : 0ULL);
}

uint64_t receiver_center_frequency_hz(const receiver_config_t *receiver, uint64_t scenario_time_ns)
{
    if (receiver_effective_mode(receiver) == RECEIVER_MODE_FIXED) {
        return receiver_fixed_center_hz(receiver);
    }

    if (!(receiver->scan_rate_hz_per_s > 0.0)) {
        return receiver->frequency_min_hz; /* validation rejects this; stay defensive */
    }
    const double span = (double)(receiver->frequency_max_hz - receiver->frequency_min_hz);
    const double t = (double)scenario_time_ns / 1000000000.0;
    const double position = fmod(receiver->scan_rate_hz_per_s * t, span);
    return receiver->frequency_min_hz + (uint64_t)llround(position);
}

uint64_t receiver_channel_center_hz(const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns)
{
    return channel->track_tuner
        ? receiver_center_frequency_hz(receiver, scenario_time_ns)
        : channel->center_frequency_hz;
}

/* A channel renders signal only while its span fits inside the front-end (ADC) window,
 * exactly like a hardware DDC that can only extract from the digitised band. A tuner-tracking
 * channel is centered on the window by construction, so containment reduces to its bandwidth. */
bool receiver_channel_in_window(const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns)
{
    if (channel->track_tuner) {
        return (uint64_t)channel->bandwidth_hz <= receiver->bandwidth_hz;
    }
    const uint64_t receiver_center = receiver_center_frequency_hz(receiver, scenario_time_ns);
    const int64_t receiver_low = (int64_t)receiver_center - (int64_t)(receiver->bandwidth_hz / 2ULL);
    const int64_t receiver_high = (int64_t)receiver_center + (int64_t)(receiver->bandwidth_hz / 2ULL);
    const int64_t channel_low = (int64_t)channel->center_frequency_hz - (int64_t)(channel->bandwidth_hz / 2U);
    const int64_t channel_high = (int64_t)channel->center_frequency_hz + (int64_t)(channel->bandwidth_hz / 2U);
    return channel_low >= receiver_low && channel_high <= receiver_high;
}

const channel_rate_t *channel_find_rate(const channel_config_t *channel, uint32_t bandwidth_hz, uint32_t sample_rate_hz)
{
    for (size_t i = 0; i < channel->rate_count; i++) {
        if (channel->rates[i].bandwidth_hz == bandwidth_hz && channel->rates[i].sample_rate_hz == sample_rate_hz) {
            return &channel->rates[i];
        }
    }
    return NULL;
}

bool receiver_validate(const receiver_config_t *receiver, char *error, size_t error_size)
{
    if (receiver->frequency_min_hz > SIM_MAX_RF_HZ || receiver->frequency_max_hz > SIM_MAX_RF_HZ ||
        receiver->frequency_max_hz <= receiver->frequency_min_hz) {
        snprintf(error, error_size,
                 "invalid_frequency: frequency_min_hz=%llu frequency_max_hz=%llu "
                 "(need min < max and both <= %llu)",
                 (unsigned long long)receiver->frequency_min_hz,
                 (unsigned long long)receiver->frequency_max_hz, (unsigned long long)SIM_MAX_RF_HZ);
        return false;
    }
    if (receiver->rest_port == 0) {
        snprintf(error, error_size, "invalid_port: rest_port=0 (must be non-zero)");
        return false;
    }
    if (receiver->bandwidth_hz == 0ULL) {
        snprintf(error, error_size, "invalid_frontend_bandwidth: bandwidth_hz=0 (must be non-zero)");
        return false;
    }
    if (receiver->output_scale <= 0.0) {
        snprintf(error, error_size, "invalid_output_scale: output_scale=%g (must be > 0)",
                 receiver->output_scale);
        return false;
    }
    /* Scan mode needs a positive sweep rate; fixed mode ignores it. */
    if (receiver_effective_mode(receiver) == RECEIVER_MODE_SCAN && !(receiver->scan_rate_hz_per_s > 0.0)) {
        snprintf(error, error_size, "invalid_scan_rate: scan_rate_hz_per_s=%g (scan mode needs > 0)",
                 receiver->scan_rate_hz_per_s);
        return false;
    }
    if (receiver->channel_count == 0 || receiver->channel_count > SIM_MAX_CHANNELS) {
        snprintf(error, error_size, "invalid_channels: channel_count=%zu (need 1..%d)",
                 receiver->channel_count, SIM_MAX_CHANNELS);
        return false;
    }
    for (size_t i = 0; i < receiver->channel_count; i++) {
        const channel_config_t *channel = &receiver->channels[i];
        if (channel->id != i) {
            snprintf(error, error_size, "invalid_channel: channel[%zu] channel_id=%u (must equal its index %zu)",
                     i, channel->id, i);
            return false;
        }
        if (channel->udp_output.port == 0) {
            snprintf(error, error_size, "invalid_channel: channel[%zu] udp_output_port=0 (must be non-zero)", i);
            return false;
        }
        if (channel->center_frequency_hz > SIM_MAX_RF_HZ) {
            snprintf(error, error_size,
                     "invalid_channel: channel[%zu] center_frequency_hz=%llu (must be <= %llu)", i,
                     (unsigned long long)channel->center_frequency_hz, (unsigned long long)SIM_MAX_RF_HZ);
            return false;
        }
        if (channel->output_scale <= 0.0) {
            snprintf(error, error_size, "invalid_channel: channel[%zu] output_scale=%g (must be > 0)", i,
                     channel->output_scale);
            return false;
        }
        if (channel->rate_count == 0 || channel->rate_count > SIM_MAX_CHANNEL_RATES) {
            snprintf(error, error_size, "channel_rates_required: channel[%zu] rate_count=%zu (need 1..%d)", i,
                     channel->rate_count, SIM_MAX_CHANNEL_RATES);
            return false;
        }
        for (size_t r = 0; r < channel->rate_count; r++) {
            const channel_rate_t *rate = &channel->rates[r];
            if (rate->bandwidth_hz == 0U || rate->sample_rate_hz < rate->bandwidth_hz) {
                snprintf(error, error_size,
                         "invalid_channel_rate: channel[%zu] rates[%zu] bandwidth_hz=%u sample_rate_hz=%u "
                         "(need bandwidth_hz > 0 and sample_rate_hz >= bandwidth_hz)",
                         i, r, rate->bandwidth_hz, rate->sample_rate_hz);
                return false;
            }
            if ((uint64_t)rate->bandwidth_hz > receiver->bandwidth_hz) {
                snprintf(error, error_size,
                         "bandwidth_exceeds_frontend: channel[%zu] rates[%zu] bandwidth_hz=%u "
                         "exceeds front-end bandwidth_hz=%llu",
                         i, r, rate->bandwidth_hz, (unsigned long long)receiver->bandwidth_hz);
                return false;
            }
        }
        /* The active pair must be one of the listed rates. */
        if (channel_find_rate(channel, channel->bandwidth_hz, channel->sample_rate_hz) == NULL) {
            snprintf(error, error_size,
                     "unsupported_channel_rate: channel[%zu] active bandwidth_hz=%u sample_rate_hz=%u "
                     "is not one of the channel's listed rates",
                     i, channel->bandwidth_hz, channel->sample_rate_hz);
            return false;
        }
    }
    snprintf(error, error_size, "ok");
    return true;
}
