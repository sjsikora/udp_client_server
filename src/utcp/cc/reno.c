#include "logging.h"
#include "utcp/cc/core.h"
#include "utcp/config.h"
#include "utcp/net/tcp.h"
#include "utcp/utcp_output.h"
#include "utils.h"
#include <stdio.h>
#include <zlog.h>

int timeout_counter = 0;

static void reno_cong_control(struct tcb *tcb, const struct cc_event_args *args) {
    switch (args->type) {
    case TCP_CC_EVENT_INIT:
        cc_init(tcb);
        break;

    case TCP_CC_EVENT_ACK:

        if (tcb->ca_state == TCP_CA_RECOVERY) {
            tcb->cwnd = tcb->ssthresh;
            tcb->ca_state = TCP_CA_OPEN;
            zlog_info(cc_logger, "ACK,%u,%u", tcb->cwnd, tcb->ssthresh);
            break;
        }

        cc_aimd(tcb, args->data.ack.acked_bytes);
        break;

    case TCP_CC_EVENT_DUP_ACK:
        if (args->data.dup.total_dups == 3) {

            // Calculate new ssthresh we use flightsize per RFC 5681 because cwnd may be well beyond rwnd.
            uint32_t flight_size = tcb->snd_nxt - tcb->snd_una;
            uint32_t half_flight = flight_size / 2;
            tcb->ssthresh = (half_flight > (2 * MSS)) ? half_flight : (2 * MSS);

            // Retransmit the lost segment
            utcp_retransmit_segment(tcb, tcb->snd_una);

            // Enter Fast Recovery
            tcb->ca_state = TCP_CA_RECOVERY;
            tcb->cwnd = tcb->ssthresh + 3 * MSS;

            dzlog_warn("Fast Retransmit: flight_size=%u, ssthresh=%u, cwnd dropped to %u", flight_size, tcb->ssthresh,
                       tcb->cwnd);
            zlog_info(cc_logger, "TRIPLE_DUP_ACK,%u,%u", tcb->cwnd, tcb->ssthresh);

        } else if (args->data.dup.total_dups > 3) {
            // Artificially inflates the ccwnd to reflect the additional segment has left the network
            tcb->cwnd += MSS;

            // Try to send new data if you can
            utcp_output(tcb);
        }

        break;

    case TCP_CC_EVENT_TIMEOUT:
        ++timeout_counter;
        cc_timeout(tcb, args->data.timeout.flight_size);
        zlog_info(cc_logger, "TIMEOUT,%u,%u", tcb->cwnd, tcb->ssthresh);

        if (timeout_counter == 2) {
            err_sys("3 timeouts");
        }
        break;
    }
}

const struct tcp_congestion_ops utcp_reno = {.name = "reno", .cong_control = reno_cong_control};
