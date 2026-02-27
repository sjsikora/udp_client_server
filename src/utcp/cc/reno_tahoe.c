#include "utcp/config.h"
#include "utcp/net/tcp.h"
#include <stdio.h>

/**
 * Init our variables in the tcb. Note, we set things
 * very high at the beginning because we want to be able to fail quickly
 * so we get a baseline for the network. Then we can go on as normal.
 */
static void cc_shared_init(struct tcb *tcb) {
    tcb->cwnd = MSS * 10;
    tcb->ssthresh = 0xFFFFFFFF;
    tcb->ca_state = TCP_CA_OPEN;
}

/**
 * On a loss event, either triple ack or timeout, we recalculate the ssthreshold.
 * This function will set ssthresh to be either half the number of bytes in flight
 * or 2 MSS. Which ever is greater.
 */
static uint32_t cc_shared_calc_ssthresh(uint32_t flight_size) {
    uint32_t half_flight = flight_size / 2;
    return (half_flight > (2 * MSS)) ? half_flight : (2 * MSS);
}

/**
 * On a successful ACK, we increase the cwnd depending if we are in slow start
 * or we are in CA phase. This function will increase the cwnd accordingly.
 */
static void cc_shared_aimd(struct tcb *tcb, uint32_t acked) {
    if (tcb->cwnd < tcb->ssthresh) {
        // Slow Start
        tcb->cwnd += acked;
    } else {
        // Congestion Avoidance (approx. 1 MSS per RTT)
        tcb->cwnd += (MSS * MSS) / tcb->cwnd;
    }
}

/**
 * On retransmission timeout, we drop down to one packet.
 */
static void cc_shared_timeout(struct tcb *tcb, uint32_t flight_size) {
    tcb->ssthresh = cc_shared_calc_ssthresh(flight_size);
    tcb->cwnd = MSS; // Hard drop to 1 MSS
    tcb->ca_state = TCP_CA_LOSS;
}

static void tahoe_cong_control(struct tcb *tcb, const struct cc_event_args *args) {
    switch (args->type) {
    case TCP_CC_EVENT_INIT:
        cc_shared_init(tcb);
        break;

    case TCP_CC_EVENT_ACK:
        tcb->ca_state = TCP_CA_OPEN; // Ensure we are out of the LOSS state
        cc_shared_aimd(tcb, args->data.ack.acked_bytes);
        break;

    case TCP_CC_EVENT_DUP_ACK:
        // Tahoe treats 3 dup ACKs as a hard loss (just like a timeout)
        if (args->data.dup.total_dups == 3) {
            uint32_t flight_size = tcb->snd_nxt - tcb->snd_una;
            cc_shared_timeout(tcb, flight_size);

            utcp_retransmit_segment(tcb, tcb->snd_una);
        }
        // Tahoe ignores dup ACKs > 3
        break;

    case TCP_CC_EVENT_TIMEOUT:
        cc_shared_timeout(tcb, args->data.timeout.flight_size);
        break;
    }
}

const struct tcp_congestion_ops utcp_tahoe = {.name = "tahoe", .cong_control = tahoe_cong_control};

static void reno_cong_control(struct tcb *tcb, const struct cc_event_args *args) {
    switch (args->type) {
    case TCP_CC_EVENT_INIT:
        cc_shared_init(tcb);
        break;

    case TCP_CC_EVENT_ACK:
        // Reno Fast Recovery Exit Logic
        if (tcb->ca_state == TCP_CA_RECOVERY) {
            tcb->cwnd = tcb->ssthresh; // Deflate the artificially inflated window
            tcb->ca_state = TCP_CA_OPEN;
        }

        // Proceed with normal growth
        cc_shared_aimd(tcb, args->data.ack.acked_bytes);
        break;

    case TCP_CC_EVENT_DUP_ACK:
        if (args->data.dup.total_dups == 3) {

            // Calculate new threshold
            uint32_t flight_size = tcb->snd_nxt - tcb->snd_una;
            tcb->ssthresh = cc_shared_calc_ssthresh(flight_size);

            // Enter Fast Recovery: Inflate window by 3 MSS for the packets that left
            tcb->cwnd = tcb->ssthresh + (3 * MSS);
            tcb->ca_state = TCP_CA_RECOVERY;

            // Try to retransmit that missing segment
            utcp_retransmit_segment(tcb, tcb->snd_una);

        } else if (args->data.dup.total_dups > 3 && tcb->ca_state == TCP_CA_RECOVERY) {
            tcb->cwnd += MSS;

            // While in fast recovery, try to transmit more data
            utcp_output(tcb);
        }
        break;
        break;

    case TCP_CC_EVENT_TIMEOUT:
        cc_shared_timeout(tcb, args->data.timeout.flight_size);
        break;
    }
}

const struct tcp_congestion_ops utcp_reno = {.name = "reno", .cong_control = reno_cong_control};
