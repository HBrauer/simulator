#ifndef WAV_READER_H
#define WAV_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t frame_count;
    float *samples;
} wav_audio_t;

bool wav_reader_load_mono_f32(const char *path, wav_audio_t *audio, char *error, size_t error_size);
void wav_audio_free(wav_audio_t *audio);

#endif
