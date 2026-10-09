#include "iq_file_reader.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

bool iq_file_reader_open(iq_file_reader_t *reader, const char *path, char *error, size_t error_size)
{
    memset(reader, 0, sizeof(*reader));
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "asset_not_found: %s: %s", path, strerror(errno));
        return false;
    }
    if (fseeko(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(error, error_size, "asset_seek_failed");
        return false;
    }
    const off_t bytes = ftello(file);
    rewind(file);
    if (bytes < 0 || ((uint64_t)bytes % sizeof(iq_ci16_t)) != 0) {
        fclose(file);
        snprintf(error, error_size, "asset_invalid_size");
        return false;
    }
    reader->file = file;
    reader->sample_count = (uint64_t)bytes / sizeof(iq_ci16_t);
    snprintf(error, error_size, "ok");
    return true;
}

void iq_file_reader_close(iq_file_reader_t *reader)
{
    if (reader->file != NULL) {
        fclose(reader->file);
        reader->file = NULL;
    }
    reader->sample_count = 0;
}

bool iq_file_reader_read(iq_file_reader_t *reader, uint64_t sample_offset, iq_ci16_t *out, size_t count, size_t *read_count)
{
    *read_count = 0;
    if (reader->file == NULL || sample_offset >= reader->sample_count) {
        return false;
    }
    const uint64_t available = reader->sample_count - sample_offset;
    const size_t wanted = available < count ? (size_t)available : count;
    if (fseeko(reader->file, (off_t)(sample_offset * sizeof(iq_ci16_t)), SEEK_SET) != 0) {
        return false;
    }
    *read_count = fread(out, sizeof(iq_ci16_t), wanted, reader->file);
    return *read_count == wanted;
}

/* Playback position within the repeat cycle, in exact integer nanosecond/sample math.
 * The earlier double fmod() on the day time could round a cycle boundary to just before
 * the end of the file instead of exactly zero, so the block starting a new cycle rendered
 * from the file's last sample and went silent for the rest of the block -- a periodic
 * one-block dropout on every repeat (visible as level pumping on decimated channels). */
bool iq_signal_active(const scenario_signal_t *signal, const scenario_source_t *source, uint64_t day_time_ns, uint64_t *sample_offset, double *offset_fraction)
{
    if (offset_fraction != NULL) {
        *offset_fraction = 0.0;
    }
    if (signal->repeat_interval_s <= 0.0 || source->sample_rate_hz == 0 || source->sample_count == 0) {
        return false;
    }
    const uint64_t start_ns = (uint64_t)llround(signal->start_time_s * 1e9);
    const uint64_t repeat_ns = (uint64_t)llround(signal->repeat_interval_s * 1e9);
    if (repeat_ns == 0ULL || day_time_ns < start_ns) {
        return false;
    }
    const uint64_t occurrence_ns = (day_time_ns - start_ns) % repeat_ns;
    /* Source-sample position = occurrence_ns * rate / 1e9, exact in 128-bit. */
    const __uint128_t scaled = (__uint128_t)occurrence_ns * (uint64_t)source->sample_rate_hz;
    const uint64_t position = (uint64_t)(scaled / 1000000000ULL);
    if (position >= source->sample_count) {
        return false; /* between the end of the file and the next repeat */
    }
    *sample_offset = position;
    if (offset_fraction != NULL) {
        *offset_fraction = (double)(uint64_t)(scaled % 1000000000ULL) / 1e9;
    }
    return true;
}

/* Loop position from the absolute output sample index rather than nanoseconds: the block grid
 * time is floor(sample_index*1e9/rate), so re-deriving samples from nanoseconds would land a
 * hair before the integer sample and push equal-rate signals onto the resampler path. Working
 * on the sample grid keeps the fraction exactly zero whenever the rates match. */
bool iq_signal_loop_position(const scenario_signal_t *signal, const scenario_source_t *source, uint64_t start_sample, uint32_t output_rate_hz, uint64_t *sample_offset, double *offset_fraction, uint64_t *samples_until_wrap)
{
    if (offset_fraction != NULL) {
        *offset_fraction = 0.0;
    }
    if (source->sample_rate_hz == 0 || source->sample_count == 0 || output_rate_hz == 0) {
        return false;
    }
    const uint64_t start_out = (uint64_t)llround(signal->start_time_s * (double)output_rate_hz);
    if (start_sample < start_out) {
        return false;
    }
    /* Source position = (start_sample - start_out) * source_rate / output_rate, exact in
     * 128-bit; the integer part wraps modulo the file length, the remainder is the [0,1)
     * fractional source-sample position. */
    const __uint128_t scaled = (__uint128_t)(start_sample - start_out) * (uint64_t)source->sample_rate_hz;
    const uint64_t remainder = (uint64_t)(scaled % output_rate_hz);
    const uint64_t offset = (uint64_t)((scaled / output_rate_hz) % source->sample_count);
    *sample_offset = offset;
    if (offset_fraction != NULL) {
        *offset_fraction = (double)remainder / (double)output_rate_hz;
    }
    if (samples_until_wrap != NULL) {
        /* Smallest n >= 1 with position(start_sample + n) reaching the next file boundary:
         * n = ceil(((sample_count - offset) * output_rate - remainder) / source_rate). Always
         * >= 1 because offset < sample_count and remainder < output_rate. */
        const __uint128_t units_to_boundary =
            (__uint128_t)(source->sample_count - offset) * output_rate_hz - remainder;
        *samples_until_wrap =
            (uint64_t)((units_to_boundary + source->sample_rate_hz - 1U) / source->sample_rate_hz);
    }
    return true;
}
