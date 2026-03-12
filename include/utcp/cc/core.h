#include "utcp/net/tcp.h"
#include <stdint.h>

/**
 * @brief Initialize a congestion algrothium.
 *
 * Sets the inital cwnd and the ssthresh to infinity to fail fast on the network.
 *
 * @note These values come from RFC 5681 standards.
 */
void cc_init(struct tcb *tcb);

/**
 * @brief Increase cwnd on normal ack
 *
 * If the cwnd is in slow start phase, we add the number of bytes acked to the cwnd,
 * if in congestion avoidance, we try to increase the cwnd 1 MSS per round trip time.
 */
void cc_aimd(struct tcb *tcb, uint32_t acked);

/**
 * @brief Calculate ssthresh by halving flight size
 *
 * On congestion events, we set the ssthresh hold to be halve of the flight
 * size. We use flight size rather than ccwnd because 1. It is better repersentation
 * on what flight size caused the network to crash, and 2. Because ccwnd may become
 * arb larger than our reciever's window.
 *
 * @note Flight size was stated in RFC 5681.
 */
uint32_t cc_halve_ssthresh(uint32_t flight_size);

/**
 * @brief Handle Timeout
 *
 * On timeout, drop ssthresh to half of the flight size, then set cwnd to one
 * single packet. Set the ca_state to TCP_CA_LOSS.
 */
void cc_timeout(struct tcb *tcb, uint32_t flight_size);
