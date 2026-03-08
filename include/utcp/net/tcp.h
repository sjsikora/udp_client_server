/**
 * This file is here to define the TCP header, the tcp state, and the
 * transmission control block.
 *
 * Everything defined here comes straight from the RFC.
 */
#ifndef TCP_H
#define TCP_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct tcb;

struct tcp_connection_id {
    uint32_t src_ip;
    uint16_t src_port;

    uint32_t dst_ip;
    uint16_t dst_port;
};

typedef struct {
    uint16_t th_sport;     /* source port */
    uint16_t th_dport;     /* destination port */
    uint32_t th_seq;       /* sequence number */
    uint32_t th_ack;       /* acknowledgement number */
    uint8_t  th_off_flags; /* upper 4 bits offset, lower 4 bits unused */
    uint8_t  th_flags;     /* note, the eideaness of the flags do not matter because they are one bit */
#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10
#define TH_URG  0x20
    uint16_t th_win; /* window */
    uint16_t th_sum; /* checksum */
    uint16_t th_urp; /* urgent pointer */
} tcphdr;

#define TCPOPT_EOL     0
#define TCPOPT_NOP     1
#define TCPOPT_WINDOW  3
#define TCPOLEN_WINDOW 3

struct tcp_segment {
    tcphdr  hdr;
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

/**
 * The size of the send buffer (data recieved from the user waiting to be sent) and
 * size of the recieve buffer (data recvieved from the send waiting for the user to
 * read) must be a power of two for wrap around logic to work.
 */
#define SEND_BUF_SIZE 262144
#define RECV_BUF_SIZE 262144

/**
 * These are the timer indexes. These four timers implement six out of seven
 * of the timers that TCP needs to track. The odd out out being the delayed ACK
 * timer. Some of these are unused currently, but for completeness, they are
 * included.
 */
#define TCPT_REXMT   0 // Retransmission
#define TCPT_PERSIST 1 // Persist (Zero Window)
#define TCPT_KEEP    2 // Keepalive / Connection Establishment
#define TCPT_2MSL    3 // 2MSL / FIN_WAIT_2

/**
 * An enum defining what congestion control state we are in.
 */
enum tcp_ca_state {
    TCP_CA_OPEN = 0, // Normal state, slow start or congestion avoidance
    TCP_CA_DISORDER, // Duplicate ACKs received, but not yet 3
    TCP_CA_CWR,      // Congestion Window Reduced (ECN)
    TCP_CA_RECOVERY, // Fast Retransmit / Fast Recovery (3+ dupacks)
    TCP_CA_LOSS      // Retransmission Timeout (RTO) occurred
};

/**
 * This enum describes the events that can be called to the CC cong_control function.
 */
enum tcp_cc_event {
    TCP_CC_EVENT_INIT = 0, // Connection just established
    TCP_CC_EVENT_ACK,      // Normal cumulative ACK for new data
    TCP_CC_EVENT_DUP_ACK,  // Duplicate ACK received (potential loss/reordering)
    TCP_CC_EVENT_TIMEOUT,  // Retransmission timer expired (severe loss)
};

struct cc_event_args {
    enum tcp_cc_event type;

    // The union holds different data depending on the event type
    union {
        // Data specifically for TCP_CC_EVENT_ACK
        struct {
            uint32_t acked_bytes;
            uint32_t rtt_us; // Your LSTM will definitely want this!
        } ack;

        // TCP_CC_EVENT_DUP_ACK
        struct {
            uint32_t total_dups; // E.g., is this the 1st or the 4th dup ACK?
        } dup;

        // Data specifically for TCP_CC_EVENT_TIMEOUT
        struct {
            uint32_t flight_size; // How much data was lost
        } timeout;
    } data;
};

/**
 * @brief A Congestion Control interface.
 */
struct tcp_congestion_ops {
    const char name[50];

    /**
     * The duties of congestion control are compelety handled by this master function.
     * The UTCP infrastructure will call this function with an event, and it's respective
     * args as defined in cc_event_args. For example, on a new ack, we pass in the number
     * of newly acked bytes. It is then up to the CC algrothium to turn follow the RFC and
     * control the ssthresh and cwnd.
     */
    void (*cong_control)(struct tcb *tcb, const struct cc_event_args *args);
};

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
    uint32_t iss;     /* the inital send sequence*/

    uint32_t irs;     /* the inital receive sequence */
    uint32_t rcv_nxt; /* next expected sequence */

    u_int32_t rcv_wnd; /* recieve window */
    u_int32_t snd_wnd; /* send window*/

    /* Internal TCB Control Flags. Won't use all these flags, but here for compeleteness from the textbook I am using */
    u_short t_flags;

#define TF_ACKNOW  0x00001 /* send ACK immediately */
#define TF_DELACK  0x00002 /* send ACK, but try to delay it */
#define TF_NODELAY 0x00004 /* Don't delay packets to coalesce (Disable Nagle) */
#define TF_NOOPT   0x00008 /* Don't use TCP options */
#define TF_SENTFIN 0x00010 /* We have sent a FIN */

    /* Congestion control */
    uint32_t cwnd;
    uint32_t ssthresh;
    uint8_t  t_dupacks; /* Number of consecutive duplicate ACKs */

    enum tcp_ca_state                ca_state; // Congestion state
    const struct tcp_congestion_ops *cc_ops;   // Pointer to the active CC algorithm

    /* Timers */

    // Each entry in the t_timer array is the number of 500-ms clock
    // ticks until the timer expires.
    short t_timer[4]; /* The four timer counters */

    uint32_t t_idle; /* The number of 500ms ticks since the last segment was received on this connection*/

    /* RTT Calculation */
    uint32_t t_rtt;    /* When a specific segment is timed, this is the ticks until that segment is acknowledged */
    uint32_t t_rtseq;  /* The starting sequence number of the segment being tracked */
    uint32_t t_srtt;   /* The average of the measured round-trip times */
    uint32_t t_rttvar; /* The variance in RTT samples */
    uint32_t t_rxtcur; /* The final calculated timeout value currently */

    uint8_t t_rxtshift; /* The number of retransmission timers that have exprired*/

    /* Send buffer */
    uint8_t  send_buf[SEND_BUF_SIZE];
    uint32_t send_buf_head; // first unacked
    uint32_t send_buf_tail; // next write position

    /* Receive buffer */
    uint8_t  recv_buf[RECV_BUF_SIZE];
    uint32_t recv_buf_head; // next byte to read
    uint32_t recv_buf_tail; // last received byte

    uint8_t snd_scale;     /* Window scale applied to incoming th_win (peer's shift) */
    uint8_t rcv_scale;     /* Window scale applied to outgoing th_win (our shift) */
    bool    scale_enabled; /* Did the peer send a window scale option in their SYN? */

    /* Mutex locks */
    pthread_mutex_t lock;     // Protects this specific TCB
    pthread_cond_t  cond_var; // Used to wake up blocking API calls (read/accept/connect)
};

#endif
