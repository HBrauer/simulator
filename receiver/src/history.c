#include "history.h"

#include <math.h>

bool waterfall_history_seconds_valid(double seconds)
{
    return isfinite(seconds) &&
           seconds >= WATERFALL_HISTORY_MIN_SECONDS &&
           seconds <= WATERFALL_HISTORY_MAX_SECONDS;
}

double waterfall_clamp_history_seconds(double seconds)
{
    if (seconds < WATERFALL_HISTORY_MIN_SECONDS) {
        return WATERFALL_HISTORY_MIN_SECONDS;
    }
    if (seconds > WATERFALL_HISTORY_MAX_SECONDS) {
        return WATERFALL_HISTORY_MAX_SECONDS;
    }
    return seconds;
}

size_t waterfall_stride_for_history(size_t rows, uint32_t sample_rate_hz, double seconds, size_t minimum_stride)
{
    if (rows == 0U || sample_rate_hz == 0U || !waterfall_history_seconds_valid(seconds)) {
        return 0U;
    }
    const double wanted = seconds * (double)sample_rate_hz / (double)rows;
    size_t stride = (size_t)(wanted + 0.5);
    if (stride < minimum_stride) {
        stride = minimum_stride;
    }
    if (stride == 0U) {
        stride = 1U;
    }
    return stride;
}

double waterfall_history_for_stride(size_t rows, uint32_t sample_rate_hz, size_t stride_samples)
{
    if (rows == 0U || sample_rate_hz == 0U || stride_samples == 0U) {
        return 0.0;
    }
    return (double)rows * (double)stride_samples / (double)sample_rate_hz;
}
