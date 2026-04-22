#ifndef UTCP_LSTM_CLIENT_H
#define UTCP_LSTM_CLIENT_H

#include <stdint.h>

/**
 * Connect to the Python LSTM inference server over a Unix domain socket.
 * Call once at program startup (after the Python server is running).
 * Returns 0 on success, -1 if the server is not reachable (non-fatal: all
 * subsequent calls become no-ops).
 */
int lstm_client_init(void);

/**
 * Send one ACK/timeout/dup-ACK event row to the Python server and drain any
 * prediction floats the server has sent back.  All socket I/O is non-blocking;
 * if the kernel buffer is full the row is silently dropped.
 *
 * The arguments match the fields logged by log_lstm_event() in logger.c.
 */
void lstm_client_send_row(uint64_t ts_us,
                          uint32_t rtt_us,    uint32_t srtt_us,
                          uint32_t rttvar_us, uint32_t rto_us,
                          uint64_t min_rtt_us,
                          int64_t  queue_delay_us,
                          int32_t  rtt_delta_us,
                          int32_t  rtt_accel_us,
                          int32_t  rto_delta_us,
                          uint32_t cwnd,       uint32_t ssthresh,
                          uint32_t snd_wnd,    uint32_t flight_size,
                          uint32_t newly_acked,
                          uint64_t inter_ack_us,
                          uint32_t ca_state,   uint32_t t_dupacks,
                          uint32_t t_rxtshift,
                          uint32_t is_dup_ack, uint32_t is_timeout);

/**
 * Returns 1 if the Python LSTM server fired on the most recent inference
 * (i.e. congestion is predicted to be imminent), 0 otherwise.
 * Always returns 0 before the first 3 seconds of data have been collected.
 */
int lstm_client_fired(void);

/** Close the socket.  Call at program exit. */
void lstm_client_close(void);

#endif /* UTCP_LSTM_CLIENT_H */
