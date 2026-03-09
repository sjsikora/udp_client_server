#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utcp/api.h>
#include <utcp/net/tcp.h>
#include <utcp/net/timers.h>
#include <utcp/utcp_init.h>
#include <utcp/utcp_output.h>
#include <utcp/utcp_utils.h>
#include <utils.h>
#include <zlog.h>

static void        handle_received_data(struct tcb *, tcphdr *, uint8_t *, ssize_t);
static ssize_t     Recvfrom(void *, size_t, int, struct sockaddr *__restrict, socklen_t *__restrict);
static void        deserialize_utcp_packet(uint8_t *, size_t, tcphdr **, uint8_t **, ssize_t *);
static struct tcb *find_tcb(tcphdr *, uint32_t);

/**
 * @brief Process the window shift option in SYN packet
 *
 * The SYN packet may have a window shift value. If it is present, put value in snd_scale
 * in the TCB. Code has been written for the possibility of processing more options, but only
 * window is implemented.
 *
 * @note This function assumes that the hdr and the data pointer are contigous in memory
 */
static void process_window_option(tcphdr *hdr, struct tcb *tcb) {
    uint8_t data_offset = (hdr->th_off_flags >> 4) * 4;
    if (data_offset > sizeof(tcphdr)) {
        uint8_t *opt_ptr = (uint8_t *)hdr + sizeof(tcphdr);
        uint8_t *opt_end = (uint8_t *)hdr + data_offset;

        while (opt_ptr < opt_end) {
            if (*opt_ptr == TCPOPT_EOL)
                break;
            if (*opt_ptr == TCPOPT_NOP) {
                opt_ptr++;
                continue;
            }

            uint8_t opt_kind = opt_ptr[0];
            uint8_t opt_len = opt_ptr[1];

            if (opt_len == 0) {
                dzlog_error("Malformed TCP option: length 0");
                break;
            }

            if (opt_kind == TCPOPT_WINDOW && opt_len == TCPOLEN_WINDOW) {
                tcb->snd_scale = opt_ptr[2];
                tcb->scale_enabled = true;
                dzlog_info("Window scaling is enabled and is %u", tcb->snd_scale);
            }
            opt_ptr += opt_len;
        }
    }
}

void *utcp_input(void *arg) {
    (void)arg;                                    // Slience compiler warning
    zlog_put_mdc("thread_name", "Listen_Thread"); // Init logging

    dzlog_info("Listen thread initialized and waiting for packets...");

    // Allocate variables we will reuse for every incoming segment
    socklen_t          fromlen;
    struct sockaddr_in from;
    fromlen = sizeof(from);

    uint8_t *buff = malloc(1500);
    ssize_t  buff_len = 1500;

    tcphdr  *hdr;
    uint8_t *data;
    ssize_t  data_length;

    for (;;) {
        // Wait for incoming packet and deserialize
        ssize_t packet_length = Recvfrom(buff, buff_len, 0, (struct sockaddr *)&from, &fromlen);
        deserialize_utcp_packet(buff, packet_length, &hdr, &data, &data_length);
        debug_print_tcp_packet(hdr, false, data, data_length);

        // Find coorsponding TCB
        struct tcb *tcb = find_tcb(hdr, ntohl(from.sin_addr.s_addr));

        pthread_mutex_lock(&tcb->lock);

        PRINT_TCP_VARS(tcb, "INPUT PRE-PROC");

        if (tcb == NULL) {
            dzlog_warn("UTCP packet came with no active socket (Drop). Src IP: %u", ntohl(from.sin_addr.s_addr));
            err_sys("UTCP packet came with no active socket");
        }

        switch (tcb->state) {
        case TCP_LISTEN: // If SYN flag set, accept new conneciton
            if (hdr->th_flags & TH_SYN) {
                dzlog_info("Received SYN from %u:%d", ntohl(from.sin_addr.s_addr), hdr->th_sport);

                process_window_option(hdr, tcb);

                tcb->dst_port = hdr->th_sport;
                tcb->dst_ip = ntohl(from.sin_addr.s_addr);
                tcb->dst_udp_port = ntohs(from.sin_port);
                tcb->irs = hdr->th_seq;
                tcb->rcv_nxt = tcb->irs + 1; // Increase sequence number by one
                tcb->snd_wnd = GET_SCALED_WIN(tcb, hdr);

                tcb->iss = 0;
                tcb->snd_nxt = tcb->iss;
                tcb->snd_una = tcb->iss;
                tcb->state = TCP_SYN_RECV;

                utcp_output(tcb);
            }
            break;

        case TCP_SYN_SENT:
            if ((hdr->th_flags & TH_SYN) && (hdr->th_flags & TH_ACK)) { // SYN-ACK Packet
                if (hdr->th_ack == tcb->snd_nxt) {                      // If the ack is ready for the next packet

                    // TCB sender window updates
                    tcb->snd_una = hdr->th_ack;     // Update new oldest unacked number
                    tcb->irs = hdr->th_seq;         // Set the server's inital recieve sequence
                    tcb->rcv_nxt = hdr->th_seq + 1; // We are now ready to recieve the (irs [or SYN bit] + 1 ) byte
                    tcb->state = TCP_ESTABLISHED;

                    process_window_option(hdr, tcb);
                    tcb->snd_wnd = GET_SCALED_WIN(tcb, hdr);

                    // Disarm the retransmission timer
                    tcb->t_timer[TCPT_REXMT] = 0;
                    tcb->t_rxtshift = 0;

                    struct cc_event_args init_args;
                    init_args.type = TCP_CC_EVENT_INIT;
                    tcb->cc_ops->cong_control(tcb, &init_args);

                    tcb->t_flags |= TF_ACKNOW;

                    utcp_output(tcb);

                    dzlog_info("Connection Established with UTCP server");
                }
            }
            break;

        case TCP_SYN_RECV:
            if ((hdr->th_flags & TH_ACK) && (hdr->th_ack == tcb->snd_nxt)) { // The final ACK of the 3-way handshake
                tcb->state = TCP_ESTABLISHED;
                tcb->snd_una = hdr->th_ack;

                // Disarm the retransmission timer
                tcb->t_timer[TCPT_REXMT] = 0;
                tcb->t_rxtshift = 0;

                struct cc_event_args init_args;
                init_args.type = TCP_CC_EVENT_INIT;
                tcb->cc_ops->cong_control(tcb, &init_args);

                dzlog_info("Handshake complete (Server side). Connection ESTABLISHED.");
            }
        // Fall through to TCP_ESTABLISHED to handle the data in the same segment
        case TCP_ESTABLISHED:
            handle_received_data(tcb, hdr, data, data_length);
            break;
        default:
            dzlog_debug("Unhandled TCP state %d in utcp_input", tcb->state);
            break;
        }

        pthread_mutex_unlock(&tcb->lock);
    }
    // Unreachable code
    free(buff);
    return NULL;
}

static void handle_received_data(struct tcb *tcb, tcphdr *hdr, uint8_t *data, ssize_t data_length) {
    /* Handle Acknowledgement */
    uint32_t ack_num = hdr->th_ack;

    if (SEQ_GT(ack_num, tcb->snd_una) && // Ensure packet isn't ACKing bytes that were already ACKed
        SEQ_LEQ(ack_num, tcb->snd_max)   // Ensure packet isn't ACKing unsent butes
    ) {
        uint32_t newly_acked_bytes = ack_num - tcb->snd_una;

        dzlog_info("VALID ACK: Advancing snd_una from %u to %u (acked %u bytes)", tcb->snd_una, ack_num,
                   newly_acked_bytes);

        // Update new oldest unacked number
        tcb->snd_una = ack_num;

        // Prevent snd_nxt from falling behind snd_una during recovery
        if (SEQ_GT(tcb->snd_una, tcb->snd_nxt)) {
            tcb->snd_nxt = tcb->snd_una;
        }

        // Clear dup ack counter
        tcb->t_dupacks = 0;

        // Slide the window over
        uint32_t old_head = tcb->send_buf_head;
        tcb->send_buf_head = tcb->send_buf_head + newly_acked_bytes;
        tcb->snd_wnd = GET_SCALED_WIN(tcb, hdr);

        dzlog_debug("Window Update: send_buf_head %u -> %u, snd_wnd set to %u", old_head, tcb->send_buf_head,
                    tcb->snd_wnd);

        // Wake up any thread blocked in utcp_send waiting for a buffer
        pthread_cond_broadcast(&tcb->cond_var);

        // If we were tracking a segment and this ACK acknowledges it then stop the timer.
        if (tcb->t_rtt != 0 && SEQ_GT(ack_num, tcb->t_rtseq)) {
            dzlog_debug("RTT Segment ACKed (seq %u). Stopping timer and updating RTO.", tcb->t_rtseq);
            // Subtract 1 because we initialized t_rtt to 1 in utcp_output
            utcp_xmit_timer(tcb, tcb->t_rtt - 1);

            tcb->t_rtt = 0;      // Clear so we can time a new segment
            tcb->t_rxtshift = 0; // Reset the exponential backoff shift on a successful ACK
        }

        // Retransmission timer
        // If this ACK acknowledges EVERYTHING we have sent, turn off the timer
        if (tcb->snd_una == tcb->snd_max) {
            dzlog_debug("All flight data ACKed. Disarming REXMT timer.");
            tcb->t_timer[TCPT_REXMT] = 0;
        } else {
            // There is still data in flight. Restart the timer for the next segment.
            dzlog_debug("Data still in flight. Restarting REXMT timer to %d ticks.", tcb->t_rxtcur);
            tcb->t_timer[TCPT_REXMT] = tcb->t_rxtcur;
        }

        struct cc_event_args args;
        args.type = TCP_CC_EVENT_ACK;
        args.data.ack.acked_bytes = newly_acked_bytes;

        tcb->cc_ops->cong_control(tcb, &args);

    } else if (ack_num == tcb->snd_una) {
        uint32_t current_scaled_win = GET_SCALED_WIN(tcb, hdr);

        /**
         * Check if this packet is a pure window update packet. This packet may contain
         * no new data, no extra acknowledgment bytes, but simply to tell us the window
         * has updated.
         */
        if (current_scaled_win > tcb->snd_wnd) {
            dzlog_info("WINDOW UPDATE: snd_wnd increased from %u to %u", tcb->snd_wnd, current_scaled_win);
            tcb->snd_wnd = current_scaled_win;

            // Wake up any application thread blocked in utcp_send waiting for window space
            pthread_cond_broadcast(&tcb->cond_var);
        }

        /* Potential Duplicate ack packet */
        else if (data_length == 0 &&                   // No data was sent in the segment
                 current_scaled_win == tcb->snd_wnd && // Send window has not been updated
                 tcb->snd_una != tcb->snd_max) {       // There is data in flight

            tcb->t_dupacks++;
            dzlog_warn("DUPLICATE ACK detected for seq %u (Count: %d). snd_max=%u", tcb->snd_una, tcb->t_dupacks,
                       tcb->snd_max);

            struct cc_event_args args;
            args.type = TCP_CC_EVENT_DUP_ACK;
            args.data.dup.total_dups = tcb->t_dupacks;

            // Pass dup ACK to the respective cong_control
            tcb->cc_ops->cong_control(tcb, &args);
        }
    }

    /* Recieve window: Handle my acknowledgment */
    if (data_length <= 0) {
        // Output data for new segments if not new data to handle.
        utcp_output(tcb);
        return;
    }

    uint32_t seq_num = hdr->th_seq;
    dzlog_debug("Processing Data Payload: seq_num=%u, length=%zd, expecting rcv_nxt=%u", seq_num, data_length,
                tcb->rcv_nxt);

    /**
     * Case that the sequence number is past our expectation.
     * This means the packet contains data that we already have correct and
     * in order, but it also may contain new data! Here, we adjust accordingly
     */
    if (SEQ_LT(seq_num, tcb->rcv_nxt)) {
        uint32_t duplicate_bytes = tcb->rcv_nxt - seq_num;

        // Case packet is full duplicate
        if (duplicate_bytes >= data_length) {
            dzlog_warn("DROP: Fully duplicate payload. Seq %u (len %zd) is strictly before rcv_nxt %u. Forcing ACK.",
                       seq_num, data_length, tcb->rcv_nxt);
            tcb->t_flags |= TF_ACKNOW;
            utcp_output(tcb);
            return;
        }

        // Case some bytes are new some are old. Trim the data down.
        dzlog_info("OVERLAP: Trimming first %u duplicate bytes from payload. seq_num: %u -> %u, datalen: %zd -> %zd",
                   duplicate_bytes, seq_num, seq_num + duplicate_bytes, data_length, data_length - duplicate_bytes);
        seq_num += duplicate_bytes;
        data += duplicate_bytes;
        data_length -= duplicate_bytes;
    }

    if (seq_num == tcb->rcv_nxt) { // Is this the packet we are expecting?

        // See how much room we have left in the buffer
        uint32_t free_space = RECV_BUF_SIZE - (tcb->recv_buf_tail - tcb->recv_buf_head);
        dzlog_debug("Buffer Check: free_space=%u, incoming_data=%zd", free_space, data_length);

        if (data_length <= (ssize_t)free_space) { // For every byte of data, copy into ring buffer
            uint32_t old_tail = tcb->recv_buf_tail;

            ring_buf_write(tcb->recv_buf, RECV_BUF_SIZE, tcb->recv_buf_tail, data, data_length);

            tcb->recv_buf_tail += data_length;
            tcb->rcv_nxt += data_length;

            dzlog_info("IN-ORDER DATA ACCEPTED: recv_buf_tail %u -> %u, rcv_nxt %u -> %u. Waking API threads.",
                       old_tail, tcb->recv_buf_tail, (uint32_t)(tcb->rcv_nxt - data_length), tcb->rcv_nxt);

            // Wake up any thread blocking in utcp_read waiting for data
            pthread_cond_broadcast(&tcb->cond_var);

            /**
             * Note, in the future, this should be replaced with a culmative
             * ack timer.
             */
            tcb->t_flags |= TF_ACKNOW;

        } else {
            // Buffer overflow: Usually, you'd drop the packet or truncate
            dzlog_error("DROP (OOM): Receive buffer full! Free space %u, but tried to insert %zd bytes.", free_space,
                        data_length);
            return;
        }
    } else {
        dzlog_warn("OUT OF ORDER: Expected %u, got %u. Dropping payload and forcing ACK.", tcb->rcv_nxt, seq_num);
        tcb->t_flags |= TF_ACKNOW;
    }

    utcp_output(tcb);
}

/**
 * @brief Deserializes incoming raw buffer into UTCP header and data pointer.
 *
 * This function is meant to be the very first function called after reciving data. The
 * data we recevice from the UDP packet will be a TCP header in network format and then
 * the data that the sender has put into the packet.
 *
 * In this function, we change the buffer to hold the tcpheader in host format and alter
 * the out_hdr and out_data to point to their respective places in the buffer.
 */
static void deserialize_utcp_packet(uint8_t *buff, size_t buf_len, tcphdr **out_hdr, uint8_t **out_data,
                                    ssize_t *out_data_len) {

    if (buf_len < sizeof(tcphdr))
        err_sys("Can not parse utcp packet. Are you sure this was sent correctly?");

    *out_hdr = (tcphdr *)buff;

    // Convert header fields from network byte order to host byte order
    (*out_hdr)->th_sport = ntohs((*out_hdr)->th_sport);
    (*out_hdr)->th_dport = ntohs((*out_hdr)->th_dport);
    (*out_hdr)->th_seq = ntohl((*out_hdr)->th_seq);
    (*out_hdr)->th_ack = ntohl((*out_hdr)->th_ack);
    (*out_hdr)->th_win = ntohs((*out_hdr)->th_win);
    (*out_hdr)->th_sum = ntohs((*out_hdr)->th_sum);
    (*out_hdr)->th_urp = ntohs((*out_hdr)->th_urp);

    *out_data = buff + sizeof(tcphdr);
    *out_data_len = buf_len - sizeof(tcphdr);
}

/**
 * @brief Find the TCB structure with four tuple or a active listening socket
 *
 */
static struct tcb *find_tcb(tcphdr *hdr, uint32_t src_ip) {
    struct tcb *listen_match = NULL;

    for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
        struct tcb *tcb_found = utcp_fd_table[i];
        if (!tcb_found)
            continue;

        if (tcb_found->src_port == hdr->th_dport && tcb_found->dst_port == hdr->th_sport &&
            tcb_found->dst_ip == src_ip) {
            return tcb_found;
        }

        if (tcb_found->state == TCP_LISTEN && tcb_found->src_port == hdr->th_dport) {
            listen_match = tcb_found;
        }
    }

    return listen_match;
}

static ssize_t Recvfrom(void *buf, size_t len, int flags, struct sockaddr *__restrict src_addr,
                        socklen_t *__restrict addrlen) {
    ssize_t data = recvfrom(udp_fd, buf, len, flags, src_addr, addrlen);

    // Cast data recieved to TCP header
    if (data < 0)
        err_sys("UTCP recvfrom failed");

    return data;
}
