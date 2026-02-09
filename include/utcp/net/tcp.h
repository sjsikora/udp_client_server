/**
 * This file is here to define the TCP header, the tcp state, and the
 * transmission control block.
 *
 * Everything defined here comes straight from the RFC.
 */
#ifndef TCP_H
#define TCP_H

#include <netinet/in.h>
#include <netinet/tcp_var.h>
#include <stdint.h>

struct tcp_connection_id {
    uint32_t src_ip;
    uint16_t src_port;

    uint32_t dst_ip;
    uint16_t dst_port;
};

typedef struct {
    uint16_t th_sport;    /* source port */
    uint16_t th_dport;    /* destination port */
    uint32_t th_seq;      /* sequence number */
    uint32_t th_ack;      /* acknowledgement number */
    uint8_t th_off_flags; /* upper 4 bits offset, lower 4 bits unused */
    uint8_t th_flags;     /* note, the eideaness of the flags do not matter because they are one bit */
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PUSH 0x08
#define TH_ACK 0x10
#define TH_URG 0x20
    uint16_t th_win; /* window */
    uint16_t th_sum; /* checksum */
    uint16_t th_urp; /* urgent pointer */
} tcphdr;

struct tcp_segment {
    tcphdr hdr;
    uint8_t data[];
};

// These states are from the TCP finite state machine
// http://tcpipguide.com/free/t_TCPOperationalOverviewandtheTCPFiniteStateMachineF-2.htm
enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_ESTABLISHED,
    TCP_CLOSE_WAIT,
    TCP_FIN_WAIT1,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT
};

#define SEND_BUF_SIZE 65535
#define RECV_BUF_SIZE 65535

/**
 * @brief Transmission Control Block (TCB)
 *
 * The Transmission Control Block (TCB) is a collection of variables for
 * each (U)TCP socket that defines the state of a TCP connection. Every
 * socket gets their own TCB, and everything you need to know about what
 * is happening is in this block.
 *
 * @note TCB should always, always, always, remain in host order.
 */
struct tcb {

    /* Connection information*/
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port; /* these ports do not map to actual port on the kernel, but a UTCP port */
    uint16_t dst_port;

    /* The real, kernel reconized, destination UDP port that we are sending to */
    uint16_t dst_udp_port;

    /* TCP socket state */
    enum tcp_state state;

    /* TCP Connection Varaibles */

    uint32_t snd_una; /* oldest unack sequence number */
    uint32_t snd_max; /* highest sequence number sent */
    uint32_t snd_nxt; /* the next sequence number to send*/
    uint32_t iss; /* the inital send sequence*/

    uint32_t irs; /* the inital receive sequence */
    uint32_t rcv_nxt; /* next expected sequence */

    u_int32_t rcv_wnd; /* recieve window */
    u_int32_t snd_wnd; /* send window*/

    /* Congestion control */
    uint32_t cwnd;
    uint32_t ssthresh;

    /* Retransmission timer */
    uint32_t rto;
    uint64_t rto_expire;

    /* Send buffer */
    uint8_t send_buf[SEND_BUF_SIZE];
    uint32_t send_buf_head; // first unacked
    uint32_t send_buf_tail; // next write position

    /* Receive buffer */
    uint8_t recv_buf[RECV_BUF_SIZE];
    uint32_t recv_buf_head; // next byte to read
    uint32_t recv_buf_tail; // last received byte

};

#endif
