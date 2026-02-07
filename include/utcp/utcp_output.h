/**
 * This file defines our tcp_output function. This function is expected to do
 * a lot of the heavy lifting when it comes to cogensition avoidence, how much
 * data to send, when to send it and managing sequence numbers and ack numbers
 */
#ifndef UTCP_OUTPUT_H
#define UTCP_OUTPUT_H

#include <utcp/net/tcp.h>

/**
 * @brief Maps TCP state to their coorspondeing flags
 *
 * The tcp_outflags is indexed by a tcp's state (tcp_state) and maps
 * to the cooresponding flags that need to be sent on an out segment.
 *
 * For example, my TCP socket starts closed. Then the client inits a connection,
 * my state changes to TCP_SYN_SENT and I grab the cooresponding flags.
 *
 * The reciving TCP socket is in a listen state, sees this segment come in, and
 * will change to a TCP_SYN_RECV state. Then, when it sends it's response, it will
 * use that state to grab the cooresponding flags.
 *
 * @note The resulting uin8_t number that returns should be placed directly on the
 * TCP header.
 */
uint8_t tcp_outflags[] = {
    TH_RST | TH_ACK,
    0,
    TH_SYN,
    TH_SYN | TH_ACK,
    TH_ACK,
    TH_ACK,
    TH_FIN | TH_ACK,
    TH_FIN | TH_ACK,
    TH_FIN | TH_ACK,
    TH_ACK,
    TH_ACK,
};

/**
 * @brief If acceptable, send a UTCP packet.
 *
 */
int utcp_output(struct tcb);

#endif
