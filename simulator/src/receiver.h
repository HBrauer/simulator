#ifndef RECEIVER_H
#define RECEIVER_H

#include "sim_types.h"

receiver_mode_t receiver_effective_mode(const receiver_config_t *receiver);
uint64_t receiver_fixed_center_hz(const receiver_config_t *receiver);
uint64_t receiver_center_frequency_hz(const receiver_config_t *receiver, uint64_t scenario_time_ns);
uint64_t receiver_channel_center_hz(const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns);
bool receiver_channel_in_window(const receiver_config_t *receiver, const channel_config_t *channel, uint64_t scenario_time_ns);
const channel_rate_t *channel_find_rate(const channel_config_t *channel, uint32_t bandwidth_hz, uint32_t sample_rate_hz);
bool receiver_validate(const receiver_config_t *receiver, char *error, size_t error_size);

#endif
