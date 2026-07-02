#ifndef WATERFALL_H
#define WATERFALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <fftw3.h>

typedef struct {
    size_t fft_size;
    size_t rows;
    fftwf_complex *samples;
    fftwf_complex *fft_out;
    fftwf_plan fft_plan;
    float *window;
    float *spectrum_db;
    float *history;
    float last_min_db;
    float last_max_db;
} waterfall_t;

bool waterfall_init(waterfall_t *wf, size_t fft_size, size_t rows);
void waterfall_free(waterfall_t *wf);
bool waterfall_push_ci16(waterfall_t *wf, const int16_t *iq, size_t sample_count);
bool waterfall_fft_size_valid(size_t fft_size);

#endif
