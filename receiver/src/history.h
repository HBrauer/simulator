#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WATERFALL_HISTORY_MIN_SECONDS 1.0
#define WATERFALL_HISTORY_DEFAULT_SECONDS 30.0
#define WATERFALL_HISTORY_MAX_SECONDS 60.0

bool waterfall_history_seconds_valid(double seconds);
size_t waterfall_stride_for_history(size_t rows, uint32_t sample_rate_hz, double seconds, size_t minimum_stride);
double waterfall_history_for_stride(size_t rows, uint32_t sample_rate_hz, size_t stride_samples);
double waterfall_clamp_history_seconds(double seconds);

#endif
