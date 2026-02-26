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
extern uint8_t tcp_outflags[];

/**
 * @brief If acceptable, send a UTCP packet.
 *
 * This function is the defacto sender of packets. Many places call to this function,
 * but it should be thought of as a request to send rather than a guarentee. This function
 * may send none, one, or many packets to the client.
 *
 * We handle this undefined packet send by calling this function on events. For example, we
 * may have a situation where the reciever's buffer is full. When the application wants to send,
 * we see that the receiver couldn't handle it, and we don't send a packet. Later, it is up to the
 * reciever to send up a packet with their new window. On that event, utcp_list`en will call tcp_ouput
 * to then process that send buffer.
 *
 * The exception to the above rule being if the t_flags attribute in the tcb is set to TF_ACKNOW.
 * In this case, we force an ACK out.
 *
 * @note IMPORTANT. utcp_output assumes that the calling thread has a lock on the calling tcb. If not,
 * then race conditions can arise.
 */
int utcp_output(struct tcb *);

#endif
