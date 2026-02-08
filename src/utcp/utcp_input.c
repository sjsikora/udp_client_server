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
    if (rb->tail >= rb->head) {
        // Space is the total size minus the occupied span
        // We subtract 1 to prevent head == tail when full
        return (rb->size - 1) - (rb->tail - rb->head);
    } else {
        return (rb->head - rb->tail) - 1;
    }
}

static ssize_t rb_filled_space(ring_buffer_t *rb) {
    if (rb->tail >= rb->head) {
        return rb->tail - rb->head;
    } else {
        return rb->size - (rb->head - rb->tail);
    }
}

ssize_t rb_write(ring_buffer_t *rb, uint8_t *src_buff, ssize_t len) {
    ssize_t free_space = rb_free_space(rb);
    if (len > free_space) {
        len = free_space; // Cap the write to available space
    }

    if (len <= 0) return 0;

    // Calculate how much we can write before reaching the end of the physical array
    ssize_t to_end = rb->size - rb->tail;
    if (len <= to_end) {
        memcpy(&(rb->data[rb->tail]), src_buff, len);
    } else {
        // Split write: fill to the end, then wrap around to the beginning
        memcpy(&(rb->data[rb->tail]), src_buff, to_end);
        memcpy(&(rb->data[0]), &src_buff[to_end], len - to_end);
    }

    rb->tail = (rb->tail + len) % rb->size;
    return len;
}

ssize_t rb_read(ring_buffer_t *rb, uint8_t *dst_buff, ssize_t len) {
    ssize_t available = rb_filled_space(rb);
    if (len > available) {
        len = available; // Cap the read to available data
    }

    if (len <= 0) return 0;

    // Calculate how much we can read before wrapping around
    ssize_t to_end = rb->size - rb->head;
    if (len <= to_end) {
        memcpy(dst_buff, &(rb->data[rb->head]), len);
    } else {
        // Split read: read to the end, then wrap to the beginning
        memcpy(dst_buff, &(rb->data[rb->head]), to_end);
        memcpy(&dst_buff[to_end], &(rb->data[0]), len - to_end);
    }

    rb->head = (rb->head + len) % rb->size;
    return len;
}

void rb_discard(ring_buffer_t *rb, ssize_t len) {
    ssize_t available = rb_filled_space(rb);
    if (len > available) {
        len = available;
    }

    rb->head = (rb->head + len) % rb->size;
}
