#include "logging.h"
#include "utcp/config.h"
#include "utcp/net/tcp.h"
#include "utcp/utcp_output.h"
#include "utils.h"
#include <stdio.h>
#include <zlog.h>

void cc_init(struct tcb *tcb) {
    tcb->cwnd = MSS * IW;
    tcb->ssthresh = 0xFFFFFFFF; // RFC 5681 specificed. Set the ssthresh arbitrarily high
    tcb->ca_state = TCP_CA_OPEN;
    dzlog_debug("CC Init: cwnd=%u, ssthresh=%u, state=OPEN", tcb->cwnd, tcb->ssthresh);
    zlog_info(cc_logger, "INIT,%u,%u", tcb->cwnd, tcb->ssthresh);
}

void cc_aimd(struct tcb *tcb, uint32_t acked) {
    uint32_t old_cwnd = tcb->cwnd;
    if (tcb->cwnd < tcb->ssthresh) {
        // We are within the Slow Start Phase
        tcb->cwnd += MIN(acked, MSS); // Per RFC 5681
        dzlog_debug("Slow Start: cwnd %u -> %u (ssthresh=%u)", old_cwnd, tcb->cwnd, tcb->ssthresh);
    } else {
        // Congestion Avoidance: RFC 5681 - increase proportional to bytes acked
        // cwnd += MSS * (acked / cwnd) -> scales correctly when ACKs cover > 1 MSS
        tcb->cwnd += MAX(((uint64_t)acked * MSS) / tcb->cwnd, 1);
        dzlog_debug("Congestion Avoidance: cwnd %u -> %u", old_cwnd, tcb->cwnd);
    }
    zlog_info(cc_logger, "ACK,%u,%u", tcb->cwnd, tcb->ssthresh);
}

uint32_t cc_halve_ssthresh(uint32_t flight_size) {
    uint32_t half_flight = flight_size / 2;
    return (half_flight > (2 * MSS)) ? half_flight : (2 * MSS);
};

void cc_timeout(struct tcb *tcb, uint32_t flight_size) {

    // If this is our first ack since timeout, we are all good
    if (tcb->ca_state == TCP_CA_LOSS) {
        tcb->ca_state = TCP_CA_OPEN;
    }

    tcb->ssthresh = cc_halve_ssthresh(flight_size);
    tcb->cwnd = MSS; // Hard drop to 1 MSS
    tcb->ca_state = TCP_CA_LOSS;
    dzlog_warn("Timeout: Hard drop! flight_size=%u, new ssthresh=%u, cwnd=%u", flight_size, tcb->ssthresh, tcb->cwnd);
}
