#include "logging.h"
#include "utcp/cc/logger.h"
#include "utcp/cc/lstm_client.h"
#include "utcp/net/tcp.h"
#include "utcp/net/timers.h"
#include "utcp/utcp_utils.h"
#include <zlog.h>

/* Clamp ssthresh to avoid the enormous initial value (0xFFFFFFFF) swamping the CSV */
#define SSTHRESH_CAP 1050000U /* 750 * MSS */

void log_lstm_event(struct tcb *tcb, uint32_t rtt_us, uint32_t newly_acked,
                    bool is_dup_ack, bool is_timeout) {
    if (!lstm_logger)
        return;

    uint64_t now_us = utcp_get_time_us();

    /* Inter-event time (0 for the very first event on this connection) */
    uint64_t inter_ack_us = (tcb->lstm_last_ts_us > 0) ? (now_us - tcb->lstm_last_ts_us) : 0;
    tcb->lstm_last_ts_us  = now_us;

    /* Convert tick-scaled TCB fields to raw microseconds */
    uint32_t srtt_us   = (tcb->t_srtt   >> 3) * (uint32_t)TCP_TICK_MS * 1000U;
    uint32_t rttvar_us = (tcb->t_rttvar >> 2) * (uint32_t)TCP_TICK_MS * 1000U;
    uint32_t rto_us    = tcb->t_rxtcur  * (uint32_t)TCP_TICK_MS * 1000U;
    uint64_t min_rtt   = tcb->min_rtt_seen_us;

    /* Queuing delay: how far above the observed minimum we are right now.
     * Only meaningful when we have a real RTT sample and an established baseline. */
    int64_t queue_delay_us = 0;
    if (rtt_us > 0 && min_rtt > 0 && (uint64_t)rtt_us > min_rtt)
        queue_delay_us = (int64_t)rtt_us - (int64_t)min_rtt;

    /* RTT first derivative (latency trend) */
    int32_t rtt_delta_us = 0;
    if (rtt_us > 0 && tcb->lstm_prev_rtt_us > 0)
        rtt_delta_us = (int32_t)rtt_us - (int32_t)tcb->lstm_prev_rtt_us;

    /* RTT second derivative (is the trend accelerating?) */
    int32_t rtt_accel_us = rtt_delta_us - tcb->lstm_prev_rtt_delta_us;

    /* RTO first derivative (captures combined SRTT + RTTVAR growth) */
    int32_t rto_delta_us = (int32_t)rto_us - (int32_t)tcb->lstm_prev_rto_us;

    /* Advance per-connection tracking state for next call */
    if (rtt_us > 0) {
        tcb->lstm_prev_rtt_delta_us = rtt_delta_us;
        tcb->lstm_prev_rtt_us       = rtt_us;
    }
    tcb->lstm_prev_rto_us = rto_us;

    /* For timeout rows, snd_nxt has NOT yet been rolled back (caller guarantees this),
     * so flight_size correctly reflects the data in-flight at the moment of loss. */
    uint32_t flight_size  = tcb->snd_nxt - tcb->snd_una;
    uint32_t ssthresh_log = (tcb->ssthresh > SSTHRESH_CAP) ? SSTHRESH_CAP : tcb->ssthresh;

    zlog_info(lstm_logger,
              /* timestamp_us, rtt_us, srtt_us, rttvar_us, rto_us, min_rtt_us */
              "%llu,%u,%u,%u,%u,%llu,"
              /* queue_delay_us, rtt_delta_us, rtt_accel_us, rto_delta_us */
              "%lld,%d,%d,%d,"
              /* cwnd, ssthresh, snd_wnd, flight_size, newly_acked */
              "%u,%u,%u,%u,%u,"
              /* inter_ack_us, ca_state, t_dupacks, t_rxtshift, is_dup_ack, is_timeout */
              "%llu,%u,%u,%u,%u,%u",
              (unsigned long long)now_us,
              rtt_us, srtt_us, rttvar_us, rto_us,
              (unsigned long long)min_rtt,
              (long long)queue_delay_us, rtt_delta_us, rtt_accel_us, rto_delta_us,
              tcb->cwnd, ssthresh_log, tcb->snd_wnd, flight_size, newly_acked,
              (unsigned long long)inter_ack_us,
              (uint32_t)tcb->ca_state, (uint32_t)tcb->t_dupacks, (uint32_t)tcb->t_rxtshift,
              (uint32_t)is_dup_ack, (uint32_t)is_timeout);

    /* Mirror the same row to the Python LSTM inference server (non-blocking).
     * This call also drains any prediction bytes waiting in the socket buffer,
     * so lstm_client_fired() reflects the latest state immediately after. */
    lstm_client_send_row(now_us,
                         rtt_us, srtt_us, rttvar_us, rto_us, min_rtt,
                         queue_delay_us, rtt_delta_us, rtt_accel_us, rto_delta_us,
                         tcb->cwnd, ssthresh_log, tcb->snd_wnd, flight_size, newly_acked,
                         inter_ack_us,
                         (uint32_t)tcb->ca_state, (uint32_t)tcb->t_dupacks,
                         (uint32_t)tcb->t_rxtshift,
                         (uint32_t)is_dup_ack, (uint32_t)is_timeout);

    /* Always log when the LSTM fires, regardless of which CC algorithm is active.
     * Acting on the prediction is the CC algorithm's responsibility. */
    if (cc_logger && lstm_client_fired())
        zlog_info(cc_logger, "LSTM_FIRED,%u,%u", tcb->cwnd, ssthresh_log);
}
