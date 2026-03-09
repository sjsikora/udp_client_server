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
#include <zlog.h>

static int pass_to_udp(struct tcp_segment *, size_t, uint32_t, uint16_t, bool);
static int utcp_send_segment(struct tcb *, uint32_t, uint8_t, size_t);

uint8_t tcp_outflags[] = {
    TH_RST | TH_ACK, 0,      TH_SYN, TH_SYN | TH_ACK, TH_ACK, TH_ACK, TH_FIN | TH_ACK, TH_FIN | TH_ACK,
    TH_FIN | TH_ACK, TH_ACK, TH_ACK,
};

/**
 * Transmits exactly one segment of previously sent data.
 * Safe to call during Fast Recovery because it does not advance snd_nxt.
 */
int utcp_retransmit_segment(struct tcb *tcb, uint32_t seq) {
    dzlog_info("--- [RETRANSMISSION REQUESTED] --- Target Seq: %u", seq);

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

    dzlog_debug("Retransmit calculations: data_bytes_sent=%u, buffered_data=%u, taking %zu bytes.", data_bytes_sent,
                buffered_data, data_length);

    // Send it! (Using the target seq, not snd_nxt)
    int bytes_sent = utcp_send_segment(tcb, seq, flags, data_length);

    PRINT_TCP_VARS(tcb, "RETRANSMIT POST-SEND");
    return bytes_sent;
}

/**
 * SO SO SO CRITICAL I AM WRITING IT TWICE
 * utcp_output assumes that the calling thread has a lock on the tcb.
 */
int utcp_output(struct tcb *tcb) {
    uint8_t flags = tcp_outflags[tcb->state];
    bool    force_send = false;

    dzlog_debug("utcp_output triggered. Initial state: %d, Base Flags: 0x%02X", tcb->state, flags);

    // Check if we have been ordered to force an ACK out
    if (tcb->t_flags & TF_ACKNOW) {
        force_send = true;
        dzlog_debug("TF_ACKNOW flag detected. Forcing packet send.");
        tcb->t_flags &= ~TF_ACKNOW; // Clear the flag immediately
    }

    int total_bytes_sent = 0;
    int segments_sent = 0;

    /**
     * We want to keep spinning and sending packets until either the reciever window is full
     * or our cwnd is full. Further development many be done on this loop to control how much
     * data is sent at a time.
     */
    while (1) {
        size_t data_length = 0;

        if (tcb->state == TCP_ESTABLISHED) {

            /**
             * Calcluate how much data we are allowed to send to the reciever right now by
             * respecting both the recievers window and the cwnd.
             */
            uint32_t send_window = (tcb->snd_wnd < tcb->cwnd) ? tcb->snd_wnd : tcb->cwnd;
            uint32_t unacked_data_in_flight = tcb->snd_nxt - tcb->snd_una;

            /**
             * This calcuates how many bytes we have sent on the wire throughtout our entire session.
             * It only includes payload, not the SYN bit (hence the - 1). We default to zero if we have
             * only sent the SYN bit
             */
            uint32_t data_bytes_sent = SEQ_GT(tcb->snd_nxt, tcb->iss) ? (tcb->snd_nxt - tcb->iss - 1) : 0;

            /**
             * Buffered data holds the number of bytes that the user has placed in our buffer that is
             * waiting to be sent.
             */
            uint32_t buffered_data = 0;
            if (tcb->send_buf_tail > data_bytes_sent) {
                buffered_data = tcb->send_buf_tail - data_bytes_sent;
            }

            dzlog_debug("Window Calc: snd_wnd=%u, cwnd=%u -> Effective Win=%u. InFlight=%u, Buffered=%u", tcb->snd_wnd,
                        tcb->cwnd, send_window, unacked_data_in_flight, buffered_data);

            // Determine how much data we can pack into this specific segment
            if (send_window > unacked_data_in_flight) {
                uint32_t can_send = send_window - unacked_data_in_flight;
                data_length = (buffered_data < can_send) ? buffered_data : can_send;

                // Clamp to MSS (Maximum Segment Size)
                if (data_length > MSS) {
                    data_length = MSS;
                }

                if (data_length > 0) {
                    dzlog_info("Preparing to send %zu bytes of payload.", data_length);
                }
            } else {
                // Window is completely full; we cannot send any more data.
                data_length = 0;
                dzlog_debug("Send window full (or blocked by cwnd). Win=%u | cwnd=%u | InFlight=%u", tcb->snd_wnd,
                            tcb->cwnd, unacked_data_in_flight);
            }
        }

        // Control Packet Safeguard
        // We only want to consume sequence space for a SYN or FIN once.
        bool is_syn_fin = (flags & (TH_SYN | TH_FIN)) != 0;
        bool sending_new_syn_fin = is_syn_fin && (tcb->snd_nxt == tcb->snd_max);

        /**
         * Don't send a packet for fun.
         *
         * If there is no data to send, we aren't sending a new SYN/FIN,
         * and we aren't explicitly forced to send an ACK, break the loop.
         */
        if (data_length == 0 && !sending_new_syn_fin && !force_send) {
            dzlog_debug("Nothing to send. Breaking output loop.");
            break;
        }

        // Send the segment
        dzlog_debug("Dispatching segment (Seq: %u, Flags: 0x%02X, DataLen: %zu)", tcb->snd_nxt, flags, data_length);
        int bytes_sent = utcp_send_segment(tcb, tcb->snd_nxt, flags, data_length);
        if (bytes_sent < 0) {
            dzlog_error("utcp_send_segment failed. Bailing out of output loop.");
            break; // Something went wrong at the UDP layer, bail out
        }

        total_bytes_sent += bytes_sent;
        segments_sent++;

        // We only advance snd_nxt if we actually sent data or a SYN/FIN bit
        if (data_length > 0 || sending_new_syn_fin) {
            uint32_t consumed = data_length + (sending_new_syn_fin ? 1 : 0);
            tcb->snd_nxt += consumed;

            // Start the retransmission timer if it isn't already running
            if (tcb->t_timer[TCPT_REXMT] == 0) {
                dzlog_debug("Arming REXMT timer to %d ticks", tcb->t_rxtcur);
                tcb->t_timer[TCPT_REXMT] = tcb->t_rxtcur;
            }

            /**
             * Start tracking this segment if it is brand new data we haven't sent before,
             * and we don't have another timer waiting for us.
             */
            if (tcb->t_rtt == 0 && (tcb->snd_nxt == tcb->snd_max)) {
                tcb->t_rtseq = tcb->snd_nxt - consumed; // Track the exact sequence number we just transmitted
                tcb->t_rtt = 1;                         // Start the slowtimo tick counter
                dzlog_debug("Started RTT tracking for seq %u", tcb->t_rtseq);
            }

            dzlog_debug("Advancing snd_nxt by %u -> New snd_nxt=%u", consumed, tcb->snd_nxt);

            if (SEQ_GT(tcb->snd_nxt, tcb->snd_max)) {
                tcb->snd_max = tcb->snd_nxt;
                dzlog_debug("Advanced snd_max to %u", tcb->snd_max);
            }
        }

        // We fulfilled the force_send requirement on the first pass, don't loop it
        force_send = false;

        // If we just sent an empty ACK or a pure SYN/FIN, we are done looping
        if (data_length == 0) {
            break;
        }
    }

    if (segments_sent > 0) {
        dzlog_info("utcp_output completed. Burst %d segments, %d total bytes.", segments_sent, total_bytes_sent);
        PRINT_TCP_VARS(tcb, "OUTPUT POST-SEND");
    }

    return total_bytes_sent;
}
/**
 * Internal helper to allocate, construct, and send a single TCP segment.
 * Calculates the buffer offset automatically based on the provided sequence number.
 */
static int utcp_send_segment(struct tcb *tcb, uint32_t seq, uint8_t flags, size_t data_length) {
    /**
     * If we have a SYN going out, we want to let the other side know our window shift
     * variable
     */
    uint8_t opt_len = 0;
    uint8_t options[4] = {0}; // Options must be padded to a 4-byte boundary

    if (flags & TH_SYN) {
        options[0] = TCPOPT_NOP;
        options[1] = TCPOPT_WINDOW;
        options[2] = TCPOLEN_WINDOW;
        options[3] = tcb->rcv_scale; // e.g., 4 (for a 2^4 = 16x multiplier)
        opt_len = 4;
    }

    size_t              segment_size = sizeof(tcphdr) + data_length + opt_len;
    struct tcp_segment *seg = malloc(segment_size);
    if (!seg)
        return -1;

    memset(seg, 0, segment_size);

    // Build the header
    seg->hdr.th_sport = htons(tcb->src_port);
    seg->hdr.th_dport = htons(tcb->dst_port);
    seg->hdr.th_seq = htonl(seq); // Use the injected sequence number
    seg->hdr.th_ack = htonl(tcb->rcv_nxt);
    seg->hdr.th_flags = flags;
    seg->hdr.th_sum = 0;

    uint8_t header_words = (sizeof(tcphdr) + opt_len) / 4;
    seg->hdr.th_off_flags = (header_words << 4) | (flags & 0x0F);

    /**
     * We don't overwrite the data in the payload because the packets that contain
     * SYN don't have payload data. If we were going to further expand the options
     * variable, we would def need a more robust handling. But since we are only
     * commuicating window size, this is good for now.
     */
    if (opt_len > 0) {
        memcpy((uint8_t *)&seg->hdr + sizeof(tcphdr), options, opt_len);
    }

    // Window calculations
    uint32_t bytes_in_buffer = tcb->recv_buf_tail - tcb->recv_buf_head;
    uint32_t current_free_space = RECV_BUF_SIZE - bytes_in_buffer;

    seg->hdr.th_win = htons(SET_SCALED_WIN(tcb, flags, current_free_space));

    // Copy payload from the ring buffer based on the specific sequence number
    if (data_length > 0) {
        uint32_t logical_offset = seq - tcb->iss - 1; // Minus one because of SYN

        ring_buf_read(tcb->send_buf, SEND_BUF_SIZE, logical_offset, seg->data, data_length);
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

    if (packet_risk_drop) {
        int result = rand_r(&random_seed);
        if ((result % 100) < 2) { // 2% chance
            dzlog_warn("MOCK NETWORK: Outgoing packet dropped! (Simulated 10%% loss)");
            return segment_size;
        }
    }
    */

    ssize_t sent_bytes = sendto(udp_fd, seg, segment_size, 0, (struct sockaddr *)&dst_addr, sizeof(dst_addr));

    if (sent_bytes == -1)
        err_sys("Something sent wrong trying to send a UDP packet");

    return sent_bytes;
}
