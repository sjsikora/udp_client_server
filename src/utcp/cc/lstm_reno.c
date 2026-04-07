#include "logging.h"
#include "utcp/cc/core.h"
#include "utcp/cc/lstm_client.h"
#include "utcp/config.h"
#include "utcp/net/tcp.h"
#include "utcp/utcp_output.h"
#include "utcp/utcp_utils.h"
#include <stdio.h>
#include <zlog.h>

static void lstm_reno_cong_control(struct tcb *tcb, const struct cc_event_args *args) {
    switch (args->type) {
    case TCP_CC_EVENT_INIT:
        cc_init(tcb);
        tcb->recover = tcb->iss;
        break;

    case TCP_CC_EVENT_ACK:
        if (tcb->ca_state == TCP_CA_RECOVERY) {
            if (SEQ_GEQ(tcb->snd_una, tcb->recover)) {
                /*
                 * Full ACK: exit recovery (RFC 6582 §3 step 3).
                 */
                tcb->cwnd    = tcb->ssthresh;
                tcb->ca_state = TCP_CA_OPEN;
                dzlog_info("LSTM-Reno: Full ACK. Exiting recovery. cwnd=%u ssthresh=%u",
                           tcb->cwnd, tcb->ssthresh);
                zlog_info(cc_logger, "ACK,%u,%u", tcb->cwnd, tcb->ssthresh);
            } else {
                /*
                 * Partial ACK: retransmit the new hole, deflate cwnd,
                 * stay in recovery (RFC 6582 §3 step 4).
                 */
                uint32_t acked = args->data.ack.acked_bytes;

                utcp_retransmit_segment(tcb, tcb->snd_una);
                tcb->t_rtt = 0;

                tcb->cwnd = (acked >= tcb->cwnd) ? MSS : tcb->cwnd - acked;
                tcb->cwnd += MSS;

                tcb->t_dupacks = 0;

                dzlog_warn("LSTM-Reno: Partial ACK. Retransmitting %u. cwnd=%u recover=%u",
                           tcb->snd_una, tcb->cwnd, tcb->recover);
                zlog_info(cc_logger, "PARTIAL_ACK,%u,%u", tcb->cwnd, tcb->ssthresh);
            }
            break;
        }

        /*
         * LSTM preemptive back-off: if the Python inference server fired,
         * reduce ssthresh to half the current flight size before growing cwnd.
         * This gives the connection a softer landing instead of waiting for
         * an actual loss event to force the reduction.
         * Only act in CA_OPEN — do not interfere with active recovery.
         */
        if (lstm_client_fired()) {
            uint32_t flight_size  = tcb->snd_nxt - tcb->snd_una;
            uint32_t new_ssthresh = cc_halve_ssthresh(flight_size);
            if (new_ssthresh < tcb->ssthresh) {
                dzlog_info("LSTM-Reno: preemptive back-off  ssthresh %u -> %u  flight=%u",
                           tcb->ssthresh, new_ssthresh, flight_size);
                zlog_info(cc_logger, "LSTM_PREEMPT,%u,%u", tcb->cwnd, new_ssthresh);
                tcb->ssthresh = new_ssthresh;
            }
        }

        cc_aimd(tcb, args->data.ack.acked_bytes);
        break;

    case TCP_CC_EVENT_DUP_ACK:
        if (args->data.dup.total_dups == 3) {
            /*
             * RFC 6582 §3 step 1: only enter fast retransmit if snd_una is
             * beyond the previous recover point.
             */
            if (SEQ_GT(tcb->snd_una, tcb->recover)) {
                uint32_t flight_size = tcb->snd_nxt - tcb->snd_una;
                tcb->ssthresh = cc_halve_ssthresh(flight_size);
                tcb->recover  = tcb->snd_max;

                utcp_retransmit_segment(tcb, tcb->snd_una);
                tcb->t_rtt = 0;

                tcb->ca_state = TCP_CA_RECOVERY;
                tcb->cwnd     = tcb->ssthresh + 3 * MSS;

                dzlog_warn("LSTM-Reno: Fast Retransmit. flight=%u ssthresh=%u cwnd=%u recover=%u",
                           flight_size, tcb->ssthresh, tcb->cwnd, tcb->recover);
                zlog_info(cc_logger, "TRIPLE_DUP_ACK,%u,%u", tcb->cwnd, tcb->ssthresh);
            } else {
                dzlog_warn("LSTM-Reno: 3 dup ACKs but snd_una=%u <= recover=%u. Skipping.",
                           tcb->snd_una, tcb->recover);
            }
        } else if (args->data.dup.total_dups > 3 && tcb->ca_state == TCP_CA_RECOVERY) {
            /* Inflate cwnd for each additional dup ACK while in recovery */
            tcb->cwnd += MSS;
            utcp_output(tcb);
        }
        break;

    case TCP_CC_EVENT_TIMEOUT:
        /*
         * On timeout, reset recover so the next 3 dup ACKs can trigger
         * fast retransmit fresh.
         */
        tcb->recover = tcb->snd_max;
        cc_timeout(tcb, args->data.timeout.flight_size);
        zlog_info(cc_logger, "TIMEOUT,%u,%u", tcb->cwnd, tcb->ssthresh);
        break;
    }
}

const struct tcp_congestion_ops utcp_lstm_reno = {
    .name         = "lstm_reno",
    .cong_control = lstm_reno_cong_control,
};
