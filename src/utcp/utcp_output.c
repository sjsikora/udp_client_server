#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utcp/config.h>
#include <utcp/net/tcp.h>
#include <utcp/net/timers.h>
#include <utcp/utcp_init.h>
#include <utcp/utcp_output.h>
#include <utcp/utcp_utils.h>
#include <utils.h>

static int pass_to_udp(struct tcp_segment *, size_t, uint32_t, uint16_t, bool);

uint8_t tcp_outflags[] = {
    TH_RST | TH_ACK, 0,      TH_SYN, TH_SYN | TH_ACK, TH_ACK, TH_ACK, TH_FIN | TH_ACK, TH_FIN | TH_ACK,
    TH_FIN | TH_ACK, TH_ACK, TH_ACK,
};

/**
 * Transmits exactly one segment of previously sent data.
 * Safe to call during Fast Recovery because it does not advance snd_nxt.
 */
int utcp_retransmit_segment(struct tcb *tcb, uint32_t seq) {
    if (tcb->state != TCP_ESTABLISHED) {
        return 0;
    }

    uint8_t flags = tcp_outflags[tcb->state];

    // Calculate how much buffered data corresponds to this sequence number
    uint32_t data_bytes_sent = (seq > tcb->iss) ? (seq - tcb->iss - 1) : 0;
    uint32_t buffered_data = 0;

    if (tcb->send_buf_tail > data_bytes_sent) {
        buffered_data = tcb->send_buf_tail - data_bytes_sent;
    }

    // We only retransmit up to 1 MSS at a time
    size_t data_length = (buffered_data < MSS) ? buffered_data : MSS;

    // Send it! (Using the target seq, not snd_nxt)
    int bytes_sent = utcp_send_segment(tcb, seq, flags, data_length);

    PRINT_TCP_VARS(tcb, "RETRANSMIT POST-SEND");
    return bytes_sent;
}

/**
 * SO SO SO CRITICAL I AM WRITING IT TWICE
 * utcp_output assumes that the calling thread has a lock the tcb.
 */
int utcp_output(struct tcb *tcb) {

    uint8_t flags = tcp_outflags[tcb->state];
    bool    force_send = false; // Do we need to force send an ACK?

    if (tcb->t_flags & TF_ACKNOW) {
        force_send = true;

        // Clear flag
        tcb->t_flags &= ~TF_ACKNOW;
    }

    size_t data_length = 0;

    if (tcb->state == TCP_ESTABLISHED) {
        uint32_t receivers_window = tcb->snd_wnd;
        uint32_t unacked_data_in_flight = tcb->snd_nxt - tcb->snd_una;

        /**
         * This calcuates how many bytes we have sent on the wire throughtout our entire session.
         * It only includes payload, not the SYN bit (hence the - 1). We default to zero if we have
         * only sent the SYN bit
         */
        uint32_t data_bytes_sent = (tcb->snd_nxt > tcb->iss) ? (tcb->snd_nxt - tcb->iss - 1) : 0;

        /**
         * Buffered data holds the number of bytes that the user has placed in our buffer that is
         * waiting to be sent.
         */
        uint32_t buffered_data = 0;
        if (tcb->send_buf_tail > data_bytes_sent) {
            buffered_data = tcb->send_buf_tail - data_bytes_sent;
        }

        if (receivers_window > unacked_data_in_flight) {
            uint32_t can_send = receivers_window - unacked_data_in_flight;

            data_length = (buffered_data < can_send) ? buffered_data : can_send;

            // Clamp to MSS (Maximum Segment Size)
            if (data_length > MSS)
                data_length = MSS;

        } else {
            printf("[DEBUG] Window Full: Win=%u | InFlight=%u\n", receivers_window, unacked_data_in_flight);
        }
    }

    int bytes_sent = utcp_send_segment(tcb, tcb->snd_nxt, flags, data_length);

    // Update TCB counters ONLY if this is new data or a SYN/FIN
    if (data_length > 0 || (flags & (TH_SYN | TH_FIN))) {
        uint32_t consumed = data_length + ((flags & (TH_SYN | TH_FIN)) ? 1 : 0);
        tcb->snd_nxt += consumed;

        if (tcb->snd_nxt > tcb->snd_max)
            tcb->snd_max = tcb->snd_nxt;

        // If the retransmission timer is not already running, start it
        if (tcb->t_timer[TCPT_REXMT] == 0) {
            tcb->t_timer[TCPT_REXMT] = TCPTV_SRTTDFLT;
        }
    }

    PRINT_TCP_VARS(tcb, "OUTPUT POST-SEND");
    return bytes_sent;
}

/**
 * Internal helper to allocate, construct, and send a single TCP segment.
 * Calculates the buffer offset automatically based on the provided sequence number.
 */
static int utcp_send_segment(struct tcb *tcb, uint32_t seq, uint8_t flags, size_t data_length) {
    size_t              segment_size = sizeof(tcphdr) + data_length;
    struct tcp_segment *seg = malloc(segment_size);
    if (!seg)
        return -1;

    memset(seg, 0, segment_size);

    // Build the header
    seg->hdr.th_sport = htons(tcb->src_port);
    seg->hdr.th_dport = htons(tcb->dst_port);
    seg->hdr.th_seq = htonl(seq); // Use the injected sequence number
    seg->hdr.th_ack = htonl(tcb->rcv_nxt);
    seg->hdr.th_off_flags = (sizeof(tcphdr) / 4) << 4;
    seg->hdr.th_flags = flags;
    seg->hdr.th_sum = 0;

    // Window calculations
    uint32_t bytes_in_buffer = tcb->recv_buf_tail - tcb->recv_buf_head;
    uint32_t current_free_space = RECV_BUF_SIZE - bytes_in_buffer;
    seg->hdr.th_win = htons((uint16_t)current_free_space);

    // Copy payload from the ring buffer based on the specific sequence number
    if (data_length > 0) {
        uint32_t buf_offset = (seq - tcb->iss - 1) % SEND_BUF_SIZE;
        memcpy(seg->data, &tcb->send_buf[buf_offset], data_length);
    }

    debug_print_tcp_packet(&seg->hdr, true, seg->data, data_length);
    int bytes_sent = pass_to_udp(seg, segment_size, tcb->dst_ip, tcb->dst_udp_port, (tcb->state == TCP_ESTABLISHED));

    free(seg);
    return bytes_sent;
}
/**
 * @brief No frills. The data that is passed is sent to the sender over UDP.
 *
 * Used as our mock IP layer.
 *
 */
static int pass_to_udp(struct tcp_segment *seg, size_t segment_size, uint32_t dst_ip, uint16_t dst_upd_port,
                       bool packet_risk_drop) {

    // NOTE: Possible optimization to cache this data.
    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(dst_upd_port);
    dst_addr.sin_addr.s_addr = htonl(dst_ip);

    /**
     * Because we are commuicating over reliable localhost, we mock a unreliable
     * network by rolling a random chance that the packet is dropped over the network.
     */
    if (packet_risk_drop) {
        int result = rand_r(&random_seed);
        if ((result % 100) < 10) { // 10% chance
            printf("[UTCP] Outgoing packet dropped!\n");
            return segment_size;
        }
    }

    ssize_t sent_bytes = sendto(udp_fd, seg, segment_size, 0, (struct sockaddr *)&dst_addr, sizeof(dst_addr));

    if (sent_bytes == -1)
        err_sys("Something sent wrong trying to send a UDP packet");

    return sent_bytes;
}
