/*
 * Defines some very helpful utility functions for the utcp
 * package. These utils should purely be for debugging and
 * ease of use. If ever these utils would break the system,
 * they belong under a specific named file/folder.
 */
#ifndef UTCP_UTILS_H
#define UTCP_UTILS_H

#include <stdbool.h>
#include <stdint.h>
#include <utcp/net/tcp.h>
#include <zlog.h>

#define SEQ_LT(a, b)  ((int)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int)((a) - (b)) >= 0)

#define PRINT_TCP_VARS(tcb, label)                                                                                     \
    dzlog_debug("[%s] STATE: %d | UNA: %u | NXT: %u | MAX: %u | SND_WND: %u | RCV_NXT: %u | RCV_WND: %u", label,       \
                (tcb)->state, (tcb)->snd_una, (tcb)->snd_nxt, (tcb)->snd_max, (tcb)->snd_wnd, (tcb)->rcv_nxt,          \
                (RECV_BUF_SIZE - ((tcb)->recv_buf_tail - (tcb)->recv_buf_head)))

/**
 * If scaling is enabled and it's not a SYN packet, shift the header window
 * left by the scale factor. Otherwise, use the raw header window.
 */
#define GET_SCALED_WIN(tcb, hdr)                                                                                       \
    (((tcb)->scale_enabled && !((hdr)->th_flags & TH_SYN)) ? ((uint32_t)(hdr)->th_win << (tcb)->snd_scale)             \
                                                           : (uint32_t)(hdr)->th_win)

/**
 * Prepares the window value for the 16-bit header field.
 * If scaling is confirmed, shift right.
 * If not, clamp to 65535 to prevent overflow.
 */
#define SET_SCALED_WIN(tcb, flags, free_space)                                                                         \
    ((tcb)->scale_enabled && !((flags) & TH_SYN) ? (uint16_t)((free_space) >> (tcb)->rcv_scale)                        \
                                                 : (uint16_t)((free_space) > 65535 ? 65535 : (free_space)))
/*
 * @brief Print out the contents of a tcp header
 */
void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered, const uint8_t *payload, size_t payload_len);

/**
 * @brief Helper function to print out the state of a tcb
 */
void dump_tcb(int fd);

/**
 * @brief Safely read from a circular ring buffer handling wrap-around.
 */
void ring_buf_read(const uint8_t *ring_buf, uint32_t buf_size, uint32_t offset, uint8_t *dst, size_t len);

/**
 * @brief Safely write to a circular ring buffer handling wrap-around.
 */
void ring_buf_write(uint8_t *ring_buf, uint32_t buf_size, uint32_t offset, const uint8_t *src, size_t len);

/**
 * @brief Returns current CLOCK_MONOTONIC time in microseconds.
 *        Used for RFC 1323 TCP Timestamp option encoding and RTT measurement.
 */
uint64_t utcp_get_time_us(void);
#endif
