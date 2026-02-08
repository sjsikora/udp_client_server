#include "ring_buffer.h"
#include <string.h>
#include <assert.h>

void ring_buffer_init(ring_buffer_t *buffer, uint8_t *buf, ssize_t buf_size) {
    buffer->data = buf;
    buffer->size = buf_size;
    buffer->head = 0;
    buffer->tail = 0;
}

ssize_t rb_free_space(ring_buffer_t *rb) {
    ssize_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    ssize_t t = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    if (t >= h) {
        return (rb->size - 1) - (t - h);
    } else {
        return (h - t) - 1;
    }
}

static ssize_t rb_filled_space(ring_buffer_t *rb) {
    size_t t = atomic_load_explicit(&rb->tail, memory_order_acquire);
    ssize_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);

    if (t >= h) {
        return t - h;
    } else {
        return rb->size - (h - t);
    }
}

ssize_t rb_write(ring_buffer_t *rb, uint8_t *src_buff, ssize_t len) {
    ssize_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    ssize_t t = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    // Calculate free space locally to avoid extra atomic loads
    ssize_t free_space = (t >= h) ? ((rb->size - 1) - (t - h)) : (h - t - 1);

    if (len > free_space) len = free_space;
    if (len <= 0) return 0;

    ssize_t to_end = rb->size - t;
    if (len <= to_end) {
        memcpy(&(rb->data[t]), src_buff, len);
    } else {
        memcpy(&(rb->data[t]), src_buff, to_end);
        memcpy(&(rb->data[0]), &src_buff[to_end], len - to_end);
    }

    atomic_store_explicit(&rb->tail, (t + len) % rb->size, memory_order_release);
    return len;
}

ssize_t rb_read(ring_buffer_t *rb, uint8_t *dst_buff, ssize_t len) {
    ssize_t t = atomic_load_explicit(&rb->tail, memory_order_acquire);
    ssize_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);

    // Calculate available space locally
    ssize_t available = (t >= h) ? (t - h) : (rb->size - (h - t));

    if (len > available) len = available;
    if (len <= 0) return 0;

    ssize_t to_end = rb->size - h;
    if (len <= to_end) {
        memcpy(dst_buff, &(rb->data[h]), len);
    } else {
        memcpy(dst_buff, &(rb->data[h]), to_end);
        memcpy(&dst_buff[to_end], &(rb->data[0]), len - to_end);
    }

    atomic_store_explicit(&rb->head, (h + len) % rb->size, memory_order_release);
    return len;
}

void rb_discard(ring_buffer_t *rb, ssize_t len) {
    ssize_t t = atomic_load_explicit(&rb->tail, memory_order_acquire);
    ssize_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);

    ssize_t available = (t >= h) ? (t - h) : (rb->size - (h - t));

    if (len > available) len = available;
    if (len <= 0) return;

    // Use RELEASE to ensure any processing of the data is finished
    // before we officially "free" the space by moving the head.
    atomic_store_explicit(&rb->head, (h + len) % rb->size, memory_order_release);
}

