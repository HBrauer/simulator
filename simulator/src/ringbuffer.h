#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t fill;
} ringbuffer_t;

bool ringbuffer_init(ringbuffer_t *rb, size_t capacity);
void ringbuffer_free(ringbuffer_t *rb);
void ringbuffer_clear(ringbuffer_t *rb);
size_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *data, size_t length);
size_t ringbuffer_read(ringbuffer_t *rb, uint8_t *data, size_t length);
size_t ringbuffer_fill(const ringbuffer_t *rb);
size_t ringbuffer_available(const ringbuffer_t *rb);

#endif
