#include "include/ring_buffer.h"

void ring_buffer_init(ring_buffer_t *buffer, uint8_t *buf, ssize_t buf_size) {
    if (!buffer || !buf) return;
    buffer->data = buf;
    buffer->size = buf_size - 1;
    buffer->tail = 0;
    buffer->head = 0;
}

ssize_t rb_free_space(ring_buffer_t *rb) {
    if (!rb) return 0;

    // We leave one byte to make sure we can distinguish from full or empty
    ssize_t used = (rb->tail - rb->head + rb->size) % rb->size;
    return rb->size - used;
}

ssize_t rb_write(ring_buffer_t *rb, uint8_t *src_buff, ssize_t len) {

    ssize_t top_index = rb->head + len;

    // Account for overflow




}
