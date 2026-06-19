#ifndef IQ_FILE_READER_H
#define IQ_FILE_READER_H

#include "sim_types.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    uint64_t sample_count;
} iq_file_reader_t;

bool iq_file_reader_open(iq_file_reader_t *reader, const char *path, uint64_t declared_sample_count, char *error, size_t error_size);
void iq_file_reader_close(iq_file_reader_t *reader);
bool iq_file_reader_read(iq_file_reader_t *reader, uint64_t sample_offset, iq_ci16_t *out, size_t count, size_t *read_count);
bool iq_signal_active(const scenario_signal_t *signal, const scenario_source_t *source, double day_time_s, uint64_t *sample_offset);

#endif
