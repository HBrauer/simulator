#include "iq_file_reader.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

bool iq_file_reader_open(iq_file_reader_t *reader, const char *path, uint64_t declared_sample_count, char *error, size_t error_size)
{
    memset(reader, 0, sizeof(*reader));
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "asset_not_found:%s", strerror(errno));
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
    const uint64_t derived_count = (uint64_t)bytes / sizeof(iq_ci16_t);
    if (declared_sample_count != 0 && declared_sample_count != derived_count) {
        fclose(file);
        snprintf(error, error_size, "asset_sample_count_mismatch");
        return false;
    }
    reader->file = file;
    reader->sample_count = derived_count;
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

bool iq_signal_active(const scenario_signal_t *signal, const scenario_source_t *source, double day_time_s, uint64_t *sample_offset, double *offset_fraction)
{
    if (offset_fraction != NULL) {
        *offset_fraction = 0.0;
    }
    if (signal->repeat_interval_s <= 0.0 || source->sample_rate_hz == 0 || source->sample_count == 0) {
        return false;
    }
    if (day_time_s < signal->start_time_s) {
        return false;
    }
    const double file_duration_s = (double)source->sample_count / (double)source->sample_rate_hz;
    const double elapsed = day_time_s - signal->start_time_s;
    const double occurrence_elapsed = fmod(elapsed, signal->repeat_interval_s);
    if (occurrence_elapsed < 0.0 || occurrence_elapsed >= file_duration_s) {
        return false;
    }
    const double exact_position = occurrence_elapsed * (double)source->sample_rate_hz;
    const double integer_position = floor(exact_position);
    *sample_offset = (uint64_t)integer_position;
    if (*sample_offset >= source->sample_count) {
        return false;
    }
    if (offset_fraction != NULL) {
        *offset_fraction = exact_position - integer_position;
    }
    return true;
}
