/**
 * This file defines a ring buffer struct. A ring buffer is a special
 * buffer where if we have a buffer of size n, the (n + 1) position will
 * point back to the 1st position. This pattern is helpful when we have
 * a single producer and consumer of our data.
 *
 * For us, that is our application layer and our TCP send and recieve buffers.
 *
 * https://embedjournal.com/assets/posts/programming/2014-05-16-implementing-circular-buffer-embedded-c/circular-buffer-animation.gif
 *
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H
#include <netinet/tcp_var.h>

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t head; // Read from here
    uint32_t tail; // Write to here
} ring_buffer_t;

// Returns actual bytes written (handles partial writes/full buffer)

/**
 * @brief Write buffer to the ring buffer
 *
 * @returns Number of bytes written to the buffer
 */
ssize_t rb_write(ring_buffer_t *rb, const uint8_t *src, uint32_t len);

// Returns actual bytes read
ssize_t rb_read(ring_buffer_t *rb, uint8_t *dst, uint32_t len);

// Useful for TCP: discard bytes once they are ACKed
void rb_discard(ring_buffer_t *rb, uint32_t len);

// How much space is left?
uint32_t rb_free_space(ring_buffer_t *rb);

#endif
