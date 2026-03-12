#include "logging.h"
#include "utcp/cc/core.h"
#include "utcp/config.h"
#include "utcp/net/tcp.h"
#include "utcp/utcp_output.h"
#include <stdio.h>
#include <zlog.h>

static void tahoe_cong_control(struct tcb *tcb, const struct cc_event_args *args) {
    switch (args->type) {
    case TCP_CC_EVENT_INIT:
        cc_init(tcb);
        break;

    case TCP_CC_EVENT_ACK:

        // Normal ack brings up back to normal CA state
        tcb->ca_state = TCP_CA_OPEN;

        // Tahoe handles ACKs normally using the shared AIMD logic.
        // It stays in Slow Start until cwnd > ssthresh, then shifts to Congestion Avoidance.
        cc_aimd(tcb, args->data.ack.acked_bytes);
        break;

    case TCP_CC_EVENT_DUP_ACK:
        if (args->data.dup.total_dups == 3) {

            uint32_t flight_size = tcb->snd_nxt - tcb->snd_una;
            tcb->ssthresh = cc_halve_ssthresh(flight_size);

            // Tahoe: Set cwnd all the way down to one segment.
            tcb->cwnd = MSS;

            zlog_info(cc_logger, "TRIPLE_DUP_ACK,%u,%u", tcb->cwnd, tcb->ssthresh);

            // Retransmit the missing segment
            utcp_retransmit_segment(tcb, tcb->snd_una);
        }
        break;

    case TCP_CC_EVENT_TIMEOUT:
        cc_timeout(tcb, args->data.timeout.flight_size);
        zlog_info(cc_logger, "TIMEOUT,%u,%u", tcb->cwnd, tcb->ssthresh);
        break;
    }
}

const struct tcp_congestion_ops utcp_tahoe = {.name = "tahoe", .cong_control = tahoe_cong_control};
