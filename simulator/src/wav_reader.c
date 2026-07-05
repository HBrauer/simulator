#include "wav_reader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static bool read_exact(FILE *file, void *dst, size_t bytes)
{
    return fread(dst, 1U, bytes, file) == bytes;
}

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_bytes;
    long data_offset;
} wav_header_t;

/* Scan the RIFF/WAVE chunks and return the PCM16 format info and data extent. Leaves the file
 * positioned wherever the scan ended (the caller seeks to data_offset before reading samples). */
static bool parse_wav_header(FILE *file, wav_header_t *out, char *error, size_t error_size)
{
    uint8_t header[12];
    if (!read_exact(file, header, sizeof(header)) || memcmp(header, "RIFF", 4U) != 0 ||
        memcmp(header + 8U, "WAVE", 4U) != 0) {
        snprintf(error, error_size, "wav_invalid_header");
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    memset(out, 0, sizeof(*out));

    while (!have_data) {
        uint8_t chunk_header[8];
        if (!read_exact(file, chunk_header, sizeof(chunk_header))) {
            break;
        }
        const uint32_t chunk_size = read_le32(chunk_header + 4U);
        const long chunk_data_offset = ftell(file);
        if (chunk_data_offset < 0) {
            snprintf(error, error_size, "wav_seek_failed");
            return false;
        }

        if (memcmp(chunk_header, "fmt ", 4U) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || !read_exact(file, fmt, sizeof(fmt))) {
                snprintf(error, error_size, "wav_invalid_fmt");
                return false;
            }
            audio_format = read_le16(fmt);
            out->channels = read_le16(fmt + 2U);
            out->sample_rate = read_le32(fmt + 4U);
            out->bits_per_sample = read_le16(fmt + 14U);
            have_fmt = true;
        } else if (memcmp(chunk_header, "data", 4U) == 0) {
            out->data_offset = chunk_data_offset;
            out->data_bytes = chunk_size;
            have_data = true;
        }

        const long next_offset = chunk_data_offset + (long)chunk_size + (long)(chunk_size & 1U);
        if (fseek(file, next_offset, SEEK_SET) != 0) {
            snprintf(error, error_size, "wav_seek_failed");
            return false;
        }
    }

    if (!have_fmt || !have_data || audio_format != 1U || (out->channels != 1U && out->channels != 2U) ||
        out->sample_rate == 0U || out->bits_per_sample != 16U) {
        snprintf(error, error_size, "wav_unsupported: need PCM16 mono/stereo");
        return false;
    }

    const uint32_t bytes_per_frame = (uint32_t)out->channels * (uint32_t)(out->bits_per_sample / 8U);
    if (bytes_per_frame == 0U || (out->data_bytes % bytes_per_frame) != 0U) {
        snprintf(error, error_size, "wav_invalid_data_size");
        return false;
    }
    return true;
}

bool wav_reader_probe(const char *path, wav_audio_t *info, char *error, size_t error_size)
{
    memset(info, 0, sizeof(*info));
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "wav_not_found:%s", strerror(errno));
        return false;
    }
    wav_header_t header;
    if (!parse_wav_header(file, &header, error, error_size)) {
        fclose(file);
        return false;
    }
    fclose(file);
    const uint32_t bytes_per_frame = (uint32_t)header.channels * (uint32_t)(header.bits_per_sample / 8U);
    info->sample_rate_hz = header.sample_rate;
    info->channels = header.channels;
    info->bits_per_sample = header.bits_per_sample;
    info->frame_count = (uint64_t)header.data_bytes / (uint64_t)bytes_per_frame;
    snprintf(error, error_size, "ok");
    return true;
}

bool wav_reader_load_mono_f32(const char *path, wav_audio_t *audio, char *error, size_t error_size)
{
    memset(audio, 0, sizeof(*audio));
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "wav_not_found:%s", strerror(errno));
        return false;
    }

    wav_header_t header;
    if (!parse_wav_header(file, &header, error, error_size)) {
        fclose(file);
        return false;
    }
    const uint16_t channels = header.channels;
    const uint32_t sample_rate = header.sample_rate;
    const uint16_t bits_per_sample = header.bits_per_sample;
    const uint32_t data_bytes = header.data_bytes;
    const long data_offset = header.data_offset;

    const uint32_t bytes_per_frame = (uint32_t)channels * (uint32_t)(bits_per_sample / 8U);
    const uint64_t frame_count = (uint64_t)data_bytes / (uint64_t)bytes_per_frame;
    if (frame_count > SIZE_MAX / sizeof(*audio->samples)) {
        fclose(file);
        snprintf(error, error_size, "wav_too_large");
        return false;
    }

    float *samples = calloc((size_t)frame_count, sizeof(*samples));
    if (samples == NULL) {
        fclose(file);
        snprintf(error, error_size, "wav_alloc_failed");
        return false;
    }
    if (fseek(file, data_offset, SEEK_SET) != 0) {
        free(samples);
        fclose(file);
        snprintf(error, error_size, "wav_seek_failed");
        return false;
    }

    for (uint64_t i = 0; i < frame_count; i++) {
        uint8_t bytes[4] = {0};
        if (!read_exact(file, bytes, bytes_per_frame)) {
            free(samples);
            fclose(file);
            snprintf(error, error_size, "wav_read_failed");
            return false;
        }
        const int16_t left = (int16_t)read_le16(bytes);
        if (channels == 1U) {
            samples[i] = (float)left / 32768.0f;
        } else {
            const int16_t right = (int16_t)read_le16(bytes + 2U);
            samples[i] = 0.5f * ((float)left + (float)right) / 32768.0f;
        }
    }

    fclose(file);
    audio->sample_rate_hz = sample_rate;
    audio->channels = channels;
    audio->bits_per_sample = bits_per_sample;
    audio->frame_count = frame_count;
    audio->samples = samples;
    snprintf(error, error_size, "ok");
    return true;
}

void wav_audio_free(wav_audio_t *audio)
{
    if (audio == NULL) {
        return;
    }
    free(audio->samples);
    memset(audio, 0, sizeof(*audio));
}
