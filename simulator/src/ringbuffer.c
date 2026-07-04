#include "ringbuffer.h"

#include <stdlib.h>
#include <string.h>

/* Slot layout: [uint64 timestamp_ns][payload record_bytes]. The 8-byte header keeps the
 * payload 8-byte aligned, which is more than enough for interleaved ci16 samples. */
#define RINGBUFFER_HEADER_BYTES (sizeof(uint64_t))

bool ringbuffer_init(ringbuffer_t *rb, size_t record_bytes, size_t capacity)
{
    if (capacity == 0) {
        return false;
    }
    rb->record_bytes = record_bytes;
    rb->stride = RINGBUFFER_HEADER_BYTES + record_bytes;
    rb->capacity = capacity;
    rb->data = calloc(capacity, rb->stride);
    if (rb->data == NULL) {
        return false;
    }
    atomic_init(&rb->head, 0);
    atomic_init(&rb->tail, 0);
    return true;
}

void ringbuffer_free(ringbuffer_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->record_bytes = 0;
    rb->stride = 0;
    rb->capacity = 0;
    atomic_store(&rb->head, 0);
    atomic_store(&rb->tail, 0);
}

size_t ringbuffer_fill(const ringbuffer_t *rb)
{
    const size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    const size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return head - tail;
}

size_t ringbuffer_available(const ringbuffer_t *rb)
{
    return rb->capacity - ringbuffer_fill(rb);
}

bool ringbuffer_try_push(ringbuffer_t *rb, uint64_t timestamp_ns, const void *payload)
{
    const size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    const size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    if (head - tail >= rb->capacity) {
        return false;
    }
    uint8_t *slot = rb->data + (head % rb->capacity) * rb->stride;
    memcpy(slot, &timestamp_ns, RINGBUFFER_HEADER_BYTES);
    memcpy(slot + RINGBUFFER_HEADER_BYTES, payload, rb->record_bytes);
    atomic_store_explicit(&rb->head, head + 1, memory_order_release);
    return true;
}

bool ringbuffer_try_pop(ringbuffer_t *rb, uint64_t *timestamp_ns, void *payload)
{
    const size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    const size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    if (head == tail) {
        return false;
    }
    const uint8_t *slot = rb->data + (tail % rb->capacity) * rb->stride;
    if (timestamp_ns != NULL) {
        memcpy(timestamp_ns, slot, RINGBUFFER_HEADER_BYTES);
    }
    memcpy(payload, slot + RINGBUFFER_HEADER_BYTES, rb->record_bytes);
    atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);
    return true;
}

void ringbuffer_drain(ringbuffer_t *rb)
{
    const size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    atomic_store_explicit(&rb->tail, head, memory_order_release);
}
