#include "wav_reader.h"
#include "test_suites.h"

#include <check.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void put_le16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xffU), f);
    fputc((int)((v >> 8U) & 0xffU), f);
}

static void put_le32(FILE *f, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        fputc((int)((v >> (8U * (unsigned)i)) & 0xffU), f);
    }
}

/* Write a WAV with a caller-chosen format tag, bits, fmt-chunk size, and data size, plus
 * `frames` mono samples. fmt_size >= 40 with tag 0xFFFE writes an EXTENSIBLE header. */
static const char *write_wav(uint16_t format_tag, uint16_t bits, uint32_t fmt_size,
                             uint16_t sub_format_tag, uint32_t data_size_field, uint32_t frames)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/wav_test_%d.wav", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NULL;
    }
    const uint32_t data_bytes = frames * (uint32_t)(bits / 8U);
    fwrite("RIFF", 1, 4, f);
    put_le32(f, 4U + (8U + fmt_size) + (8U + data_bytes));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_le32(f, fmt_size);
    put_le16(f, format_tag);
    put_le16(f, 1);        /* channels */
    put_le32(f, 48000);    /* sample rate */
    put_le32(f, 48000U * (bits / 8U));
    put_le16(f, (uint16_t)(bits / 8U));
    put_le16(f, bits);
    for (uint32_t i = 16; i < fmt_size; i++) {
        if (i == 24) {
            put_le16(f, sub_format_tag); /* first 2 bytes of SubFormat GUID */
            i++;
        } else {
            fputc(0, f);
        }
    }
    fwrite("data", 1, 4, f);
    put_le32(f, data_size_field);
    for (uint32_t i = 0; i < data_bytes; i++) {
        fputc((int)(i & 0xffU), f);
    }
    fclose(f);
    return path;
}

START_TEST(accepts_standard_pcm16)
{
    const char *p = write_wav(1, 16, 16, 0, 8, 4);
    ck_assert_ptr_nonnull(p);
    wav_audio_t info;
    char err[128];
    ck_assert_msg(wav_reader_probe(p, &info, err, sizeof(err)), "%s", err);
    ck_assert_uint_eq(info.sample_rate_hz, 48000);
    ck_assert_uint_eq(info.frame_count, 4);
    unlink(p);
}
END_TEST

START_TEST(accepts_extensible_pcm16)
{
    /* EXTENSIBLE wrapper (tag 0xFFFE, 40-byte fmt) around PCM16. */
    const char *p = write_wav(0xFFFE, 16, 40, 1, 8, 4);
    ck_assert_ptr_nonnull(p);
    wav_audio_t info;
    char err[128];
    ck_assert_msg(wav_reader_probe(p, &info, err, sizeof(err)), "%s", err);
    ck_assert_uint_eq(info.frame_count, 4);
    unlink(p);
}
END_TEST

START_TEST(rejects_float32_with_clear_error)
{
    const char *p = write_wav(3, 32, 16, 0, 16, 4); /* IEEE float */
    ck_assert_ptr_nonnull(p);
    wav_audio_t info;
    char err[128];
    ck_assert(!wav_reader_probe(p, &info, err, sizeof(err)));
    ck_assert(strstr(err, "wav_unsupported_format") != NULL);
    unlink(p);
}
END_TEST

START_TEST(handles_streamed_data_size_0xffffffff)
{
    const char *p = write_wav(1, 16, 16, 0, 0xFFFFFFFFU, 6);
    ck_assert_ptr_nonnull(p);
    wav_audio_t info;
    char err[128];
    ck_assert_msg(wav_reader_probe(p, &info, err, sizeof(err)), "%s", err);
    ck_assert_uint_eq(info.frame_count, 6); /* derived from actual file size */
    unlink(p);
}
END_TEST

Suite *wav_reader_suite(void)
{
    Suite *suite = suite_create("wav_reader");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, accepts_standard_pcm16);
    tcase_add_test(tc, accepts_extensible_pcm16);
    tcase_add_test(tc, rejects_float32_with_clear_error);
    tcase_add_test(tc, handles_streamed_data_size_0xffffffff);
    suite_add_tcase(suite, tc);
    return suite;
}
