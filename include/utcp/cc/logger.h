#ifndef UTCP_CC_LOGGER_H
#define UTCP_CC_LOGGER_H

#include <stdbool.h>
#include <stdint.h>
#include <utcp/net/tcp.h>

/**
 * @brief Log one row of LSTM training data to the lstm_data CSV.
 *
 * Call sites (always called with the TCB lock held):
 *   1. handle_received_data() — after valid ACK CC callback fires
 *      rtt_us = measured RTT (0 if no sample this ACK), newly_acked = bytes acked
 *   2. handle_received_data() — after duplicate ACK CC callback fires
 *      rtt_us = 0, newly_acked = 0, is_dup_ack = true
 *   3. utcp_timers() TCPT_REXMT — BEFORE snd_nxt rollback and CC callback
 *      rtt_us = 0, newly_acked = 0, is_timeout = true
 *      (flight_size is read from TCB before rollback by the logger itself)
 *
 * CSV columns (raw values; normalization done offline in Python):
 *   timestamp_us, rtt_us, srtt_us, rttvar_us, rto_us, min_rtt_us,
 *   queue_delay_us, rtt_delta_us, rtt_accel_us, rto_delta_us,
 *   cwnd, ssthresh, snd_wnd, flight_size, newly_acked,
 *   inter_ack_us, ca_state, t_dupacks, t_rxtshift, is_dup_ack, is_timeout
 */
void log_lstm_event(struct tcb *tcb, uint32_t rtt_us, uint32_t newly_acked,
                    bool is_dup_ack, bool is_timeout);

#endif /* UTCP_CC_LOGGER_H */
