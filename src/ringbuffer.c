#include "ringbuffer.h"

#include <stdlib.h>
#include <string.h>

bool ringbuffer_init(ringbuffer_t *rb, size_t capacity)
{
    rb->data = calloc(1, capacity);
    if (rb->data == NULL) {
        return false;
    }
    rb->capacity = capacity;
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->fill = 0;
    return true;
}

void ringbuffer_free(ringbuffer_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->capacity = 0;
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->fill = 0;
}

void ringbuffer_clear(ringbuffer_t *rb)
{
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->fill = 0;
}

size_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *data, size_t length)
{
    const size_t writable = rb->capacity - rb->fill;
    const size_t n = length < writable ? length : writable;
    for (size_t i = 0; i < n; i++) {
        rb->data[rb->write_pos] = data[i];
        rb->write_pos = (rb->write_pos + 1) % rb->capacity;
    }
    rb->fill += n;
    return n;
}

size_t ringbuffer_read(ringbuffer_t *rb, uint8_t *data, size_t length)
{
    const size_t n = length < rb->fill ? length : rb->fill;
    for (size_t i = 0; i < n; i++) {
        data[i] = rb->data[rb->read_pos];
        rb->read_pos = (rb->read_pos + 1) % rb->capacity;
    }
    rb->fill -= n;
    return n;
}

size_t ringbuffer_fill(const ringbuffer_t *rb)
{
    return rb->fill;
}

size_t ringbuffer_available(const ringbuffer_t *rb)
{
    return rb->capacity - rb->fill;
}
