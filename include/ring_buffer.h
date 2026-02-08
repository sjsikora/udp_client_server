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
    ssize_t size;
    ssize_t head;
    ssize_t tail;
} ring_buffer_t;

/**
 * @brief Initializes the ring buffer
 *
 * The resulting buffer will be used as the ring buffer. The resulting
 * ring buffer will be of (buf_size - 1).
 */
void ring_buffer_init(ring_buffer_t *buffer, uint8_t *buf, ssize_t buf_size);

/**
 * @brief Write buffer to the ring buffer
 *
 * @returns Number of the bytes successfully written to the buffer
 */
ssize_t rb_write(ring_buffer_t *rb, uint8_t *src_buff, ssize_t len);

/**
 * @brief Read ring buffer
 *
 * @return Number of the bytes read successfully
 */
ssize_t rb_read(ring_buffer_t *rb, uint8_t *dst_buff, ssize_t len);

/**
 * @brief Discard the tail end of the buffer
 *
 * Increases the head pointer without reading the bytes.
 */
void rb_discard(ring_buffer_t *rb, ssize_t len);

/**
 * @brief Returns how many many bytes are free in the array.
 *
 * @return Number of bytes free
 */
ssize_t rb_free_space(ring_buffer_t *rb);

#endif
