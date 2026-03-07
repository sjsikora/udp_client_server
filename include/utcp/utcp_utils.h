/*
 * Defines some very helpful utility functions for the utcp
 * package. These utils should purely be for debugging and
 * ease of use. If ever these utils would break the system,
 * they belong under a specific named file/folder.
 */
#ifndef UTCP_UTILS_H
#define UTCP_UTILS_H

#include <stdbool.h>
#include <utcp/net/tcp.h>
#include <zlog.h>

#define SEQ_LT(a, b)  ((int)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int)((a) - (b)) >= 0)

#define PRINT_TCP_VARS(tcb, label)                                                                                     \
    dzlog_debug("[%s] STATE: %d | UNA: %u | NXT: %u | MAX: %u | SND_WND: %u | RCV_NXT: %u | RCV_WND: %u\n", label,     \
                (tcb)->state, (tcb)->snd_una, (tcb)->snd_nxt, (tcb)->snd_max, (tcb)->snd_wnd, (tcb)->rcv_nxt,          \
                (RECV_BUF_SIZE - ((tcb)->recv_buf_tail - (tcb)->recv_buf_head)))

/*
 * @brief Print out the contents of a tcp header
 */
void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered, const uint8_t *payload, size_t payload_len);

/*
 * @brief Helper function to print out the state of a tcb
 */
void dump_tcb(int fd);

#endif
