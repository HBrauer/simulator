#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single-producer / single-consumer record ring.
 *
 * Exactly one thread pushes and one thread pops; no mutex is required. Each record carries a
 * scenario timestamp alongside its payload so a rendered block keeps the timestamp of the
 * scenario time it was rendered for, all the way to the wire. Head/tail are free-running
 * counters (never wrapped) published with release / consumed with acquire ordering. */
typedef struct {
    uint8_t *data;
    size_t record_bytes;   /* payload bytes per record */
    size_t stride;         /* bytes per slot: header + payload */
    size_t capacity;       /* number of record slots */
    _Atomic size_t head;   /* producer cursor (records ever pushed) */
    _Atomic size_t tail;   /* consumer cursor (records ever popped) */
} ringbuffer_t;

bool ringbuffer_init(ringbuffer_t *rb, size_t record_bytes, size_t capacity);
void ringbuffer_free(ringbuffer_t *rb);
size_t ringbuffer_fill(const ringbuffer_t *rb);        /* records available to pop */
size_t ringbuffer_available(const ringbuffer_t *rb);   /* free record slots */

/* Producer side. Returns false if the ring is full. */
bool ringbuffer_try_push(ringbuffer_t *rb, uint64_t timestamp_ns, const void *payload);
/* Consumer side. Returns false if the ring is empty. */
bool ringbuffer_try_pop(ringbuffer_t *rb, uint64_t *timestamp_ns, void *payload);
/* Consumer side only: discard all currently-buffered records. */
void ringbuffer_drain(ringbuffer_t *rb);

#endif
