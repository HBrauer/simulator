#include "waterfall.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool waterfall_fft_size_valid(size_t fft_size)
{
    return fft_size >= 16U && fft_size <= 65536U && (fft_size & (fft_size - 1U)) == 0U;
}

bool waterfall_init(waterfall_t *wf, size_t fft_size, size_t rows)
{
    if (wf == 0 || !waterfall_fft_size_valid(fft_size) || rows == 0U) {
        return false;
    }
    memset(wf, 0, sizeof(*wf));
    wf->fft_size = fft_size;
    wf->rows = rows;
    wf->samples = fftwf_alloc_complex(fft_size);
    wf->fft_out = fftwf_alloc_complex(fft_size);
    wf->window = calloc(fft_size, sizeof(*wf->window));
    wf->spectrum_db = calloc(fft_size, sizeof(*wf->spectrum_db));
    wf->history = calloc(rows * fft_size, sizeof(*wf->history));
    if (wf->samples == 0 || wf->fft_out == 0 || wf->window == 0 || wf->spectrum_db == 0 || wf->history == 0) {
        waterfall_free(wf);
        return false;
    }
    wf->fft_plan = fftwf_plan_dft_1d((int)fft_size, wf->samples, wf->fft_out, FFTW_FORWARD, FFTW_MEASURE);
    if (wf->fft_plan == 0) {
        waterfall_free(wf);
        return false;
    }
    double window_sum = 0.0;
    for (size_t i = 0; i < fft_size; i++) {
        wf->window[i] = 0.5f - 0.5f * (float)cos((2.0 * M_PI * (double)i) / (double)(fft_size - 1U));
        window_sum += (double)wf->window[i];
    }
    /* An on-bin full-scale complex tone (|z| = 1 after the 1/32768 normalization) produces an
     * FFT-bin magnitude of sum(window). Subtracting 20*log10 of that puts full scale at 0 dBFS. */
    wf->full_scale_db = 20.0f * log10f((float)window_sum);
    for (size_t i = 0; i < rows * fft_size; i++) {
        wf->history[i] = -120.0f;
    }
    for (size_t i = 0; i < fft_size; i++) {
        wf->spectrum_db[i] = -120.0f;
    }
    wf->last_min_db = -120.0f;
    wf->last_max_db = -120.0f;
    return true;
}

void waterfall_free(waterfall_t *wf)
{
    if (wf == 0) {
        return;
    }
    if (wf->fft_plan != 0) {
        fftwf_destroy_plan(wf->fft_plan);
    }
    fftwf_free(wf->samples);
    fftwf_free(wf->fft_out);
    free(wf->window);
    free(wf->spectrum_db);
    free(wf->history);
    memset(wf, 0, sizeof(*wf));
}

bool waterfall_push_ci16(waterfall_t *wf, const int16_t *iq, size_t sample_count)
{
    if (wf == 0 || iq == 0 || sample_count < wf->fft_size || wf->fft_plan == 0) {
        return false;
    }
    for (size_t i = 0; i < wf->fft_size; i++) {
        wf->samples[i][0] = ((float)iq[2U * i] / 32768.0f) * wf->window[i];
        wf->samples[i][1] = ((float)iq[2U * i + 1U] / 32768.0f) * wf->window[i];
    }
    fftwf_execute(wf->fft_plan);

    float *row = wf->history + wf->next_row * wf->fft_size;
    const size_t half = wf->fft_size / 2U;
    float row_min_db = 1000000.0f;
    float row_max_db = -1000000.0f;
    for (size_t i = 0; i < wf->fft_size; i++) {
        const size_t src = (i + half) & (wf->fft_size - 1U);
        const float re = wf->fft_out[src][0];
        const float im = wf->fft_out[src][1];
        const float mag = sqrtf(re * re + im * im) + 1.0e-12f;
        wf->spectrum_db[i] = 20.0f * log10f(mag) - wf->full_scale_db;
        row[i] = wf->spectrum_db[i];
        if (row[i] < row_min_db) {
            row_min_db = row[i];
        }
        if (row[i] > row_max_db) {
            row_max_db = row[i];
        }
    }
    wf->last_min_db = row_min_db;
    wf->last_max_db = row_max_db;
    wf->next_row = (wf->next_row + 1U) % wf->rows;
    return true;
}
