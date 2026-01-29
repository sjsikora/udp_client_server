/**
 * This file defines structs that are deal with the TCP portcol.
 * Anything described here could be found in the RFC. It has nothing
 * to do with our own hombrewed structs or logic.
 */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <netinet/in.h>


struct tcp_connection_id {
    uint32_t src_ip;
    uint16_t src_port;

    uint32_t dst_ip;
    uint16_t dst_port;
};


typedef struct
{
    uint16_t th_sport;              /* source port */
    uint16_t th_dport;                /* destination port */
    uint32_t th_seq;                /* sequence number */
    uint32_t th_ack;                /* acknowledgement number */
    uint8_t th_off_flags;           /* upper 4 bits offset, lower 4 bits unused */
    uint8_t th_flags;
    #  define TH_FIN        0x01
    #  define TH_SYN        0x02
    #  define TH_RST        0x04
    #  define TH_PUSH        0x08
    #  define TH_ACK        0x10
    #  define TH_URG        0x20
    uint16_t th_win;                /* window */
    uint16_t th_sum;                /* checksum */
    uint16_t th_urp;                /* urgent pointer */
} tcphdr;

struct tcp_segment
{
    tcphdr hdr;
    uint8_t data[];
};

// These states are from the TCP finite state machine http://tcpipguide.com/free/t_TCPOperationalOverviewandtheTCPFiniteStateMachineF-2.htm
enum
{
    TCP_ESTABLISHED = 1,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_LISTEN,
    TCP_CLOSING
};

struct tcb_info
{
    struct tcp_connection_id id; /* the unquie 4 tuple that defines the tcp connection */
    uint8_t state;              /* state of the tcp port (see tcp state machine enum) */

    uint32_t snd_una;               /* oldest unack sequence number */
    uint32_t snd_nxt;               /* the next sequence number to send */
    uint32_t iss;               /* the inital send sequence */
    uint32_t rcv_nxt;               /* the next expected sequence */
    uint32_t irs;               /* initial recv seq */
};


#endif
