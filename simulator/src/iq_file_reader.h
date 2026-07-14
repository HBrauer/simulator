#ifndef IQ_FILE_READER_H
#define IQ_FILE_READER_H

#include "sim_types.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    uint64_t sample_count;
} iq_file_reader_t;

bool iq_file_reader_open(iq_file_reader_t *reader, const char *path, char *error, size_t error_size);
void iq_file_reader_close(iq_file_reader_t *reader);
bool iq_file_reader_read(iq_file_reader_t *reader, uint64_t sample_offset, iq_ci16_t *out, size_t count, size_t *read_count);
/* Reports whether a signal is playing at the given time-of-day. On success returns the source
 * playback position split into an integer *sample_offset and a [0,1) *offset_fraction, so the
 * renderer can start the resampler at the exact fractional sample and avoid per-block timing
 * jitter. offset_fraction may be NULL if the caller does not need the fractional part. */
bool iq_signal_active(const scenario_signal_t *signal, const scenario_source_t *source, uint64_t day_time_ns, uint64_t *sample_offset, double *offset_fraction);
/* Playback position of a continuously looping signal at the absolute output sample index
 * `start_sample` (on the output_rate_hz grid). The position is a pure function of the grid
 * index, so independently started instances agree sample-for-sample, and the modulo wrap at
 * the file end is exact. For equal source/output rates the fraction is always 0, keeping the
 * direct (no-resample) render paths. Returns false before the signal's start_time_s.
 * samples_until_wrap (nullable) receives the number of output samples (>= 1) until the source
 * position reaches the file end, so the renderer can split a block at the loop seam. */
bool iq_signal_loop_position(const scenario_signal_t *signal, const scenario_source_t *source, uint64_t start_sample, uint32_t output_rate_hz, uint64_t *sample_offset, double *offset_fraction, uint64_t *samples_until_wrap);

#endif
