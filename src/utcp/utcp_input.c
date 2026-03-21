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
static void        insert_ooo_segment(struct tcb *, uint32_t, uint8_t *, uint32_t);
static void        drain_ooo_queue(struct tcb *);
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

            if (opt_ptr + opt_len > opt_end) {
                dzlog_error("Malformed TCP option: extends past header boundary");
                break;
            }

            if (opt_kind == TCPOPT_WINDOW && opt_len == TCPOLEN_WINDOW) {
                tcb->snd_scale = opt_ptr[2];
                tcb->scale_enabled = true;
                dzlog_info("Window scaling is enabled and is %u", tcb->snd_scale);
            }

            if (opt_kind == TCPOPT_TIMESTAMP && opt_len == TCPOLEN_TIMESTAMP) {
                /* opt_ptr[2..5] = TSval from peer; store it to echo back as TSecr */
                tcb->ts_recent = ((uint32_t)opt_ptr[2] << 24) | ((uint32_t)opt_ptr[3] << 16)
                               | ((uint32_t)opt_ptr[4] <<  8) |  (uint32_t)opt_ptr[5];
                tcb->ts_enabled = true;
                dzlog_info("TS option parsed: ts_recent=%u", tcb->ts_recent);
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

/**
 * @brief Insert an out-of-order segment into the TCB's reassembly queue.
 *
 * The queue is kept sorted in ascending sequence-number order.  Overlapping
 * and duplicate bytes are trimmed so the queue never holds redundant data.
 * ooo_bytes counts against the effective receive window to prevent the sender
 * from overrunning the buffer.
 *
 * Follows the BSD 4.4 TCP reassembly approach (sys/netinet/tcp_input.c).
 *
 * @note Called with TCB lock held.
 */
static void insert_ooo_segment(struct tcb *tcb, uint32_t seq, uint8_t *data, uint32_t len) {
    /* Available space: what isn't already used by in-order or OOO bytes */
    uint32_t buf_used = tcb->recv_buf_tail - tcb->recv_buf_head;
    uint32_t available = RECV_BUF_SIZE - buf_used - tcb->ooo_bytes;

    if (len > available) {
        dzlog_warn("OOO DROP (OOM): No buffer space. buf_used=%u ooo_bytes=%u incoming=%u", buf_used, tcb->ooo_bytes,
                   len);
        return;
    }

    uint32_t end_seq = seq + len; /* exclusive end of incoming segment */

    /* Walk to the insertion point: skip entries that end before our segment starts */
    struct tcpq_entry *prev = NULL;
    struct tcpq_entry *cur = tcb->ooo_head;

    while (cur != NULL && SEQ_LEQ(cur->seq + cur->len, seq)) {
        prev = cur;
        cur = cur->next;
    }

    /* Trim start: the tail of the previous entry may overlap our new segment */
    if (prev != NULL) {
        uint32_t prev_end = prev->seq + prev->len;
        if (SEQ_GT(prev_end, seq)) {
            uint32_t overlap = prev_end - seq;
            if (overlap >= len) {
                dzlog_debug("OOO: seg [%u,%u) fully covered by existing entry. Discarding.", seq, end_seq);
                return;
            }
            data += overlap;
            len -= overlap;
            seq = prev_end;
            end_seq = seq + len;
        }
    }

    /* Allocate new queue entry */
    struct tcpq_entry *entry = malloc(sizeof(struct tcpq_entry));
    if (!entry) {
        dzlog_error("OOO: malloc failed for tcpq_entry");
        return;
    }
    entry->data = malloc(len);
    if (!entry->data) {
        free(entry);
        dzlog_error("OOO: malloc failed for OOO data buffer");
        return;
    }
    memcpy(entry->data, data, len);
    entry->seq = seq;
    entry->len = len;
    entry->next = NULL;

    /* Absorb or trim any following entries that our new segment overlaps */
    while (cur != NULL && SEQ_LT(cur->seq, end_seq)) {
        uint32_t cur_end = cur->seq + cur->len;
        if (SEQ_LEQ(cur_end, end_seq)) {
            /* cur is fully covered — remove it */
            struct tcpq_entry *next = cur->next;
            tcb->ooo_bytes -= cur->len;
            free(cur->data);
            free(cur);
            cur = next;
        } else {
            /* cur partially overlaps — trim its front */
            uint32_t overlap = end_seq - cur->seq;
            memmove(cur->data, cur->data + overlap, cur->len - overlap);
            tcb->ooo_bytes -= overlap;
            cur->seq += overlap;
            cur->len -= overlap;
            break;
        }
    }

    /* Link the new entry into the sorted list */
    entry->next = cur;
    if (prev == NULL) {
        tcb->ooo_head = entry;
    } else {
        prev->next = entry;
    }
    tcb->ooo_bytes += len;

    dzlog_info("OOO BUFFERED: seg [%u, %u). ooo_bytes now %u", seq, end_seq, tcb->ooo_bytes);
}

/**
 * @brief Drain consecutive entries from the OOO queue into the receive buffer.
 *
 * Called after an in-order segment is accepted.  If the head of the OOO queue
 * now begins exactly at rcv_nxt, that data is contiguous and can be moved into
 * recv_buf.  We repeat until the queue is empty or a gap remains, advancing
 * rcv_nxt cumulatively (i.e. a single cumulative ACK covers all drained data).
 *
 * @note Called with TCB lock held.
 */
static void drain_ooo_queue(struct tcb *tcb) {
    while (tcb->ooo_head != NULL) {
        struct tcpq_entry *entry = tcb->ooo_head;
        uint32_t           entry_end = entry->seq + entry->len;

        /* Case 1: Fully redundant — the entire entry falls below rcv_nxt.
         * This can happen when a large in-order segment overlaps data that
         * was already sitting in the OOO queue. Discard and keep scanning;
         * the next entry may still be useful. */
        if (SEQ_LEQ(entry_end, tcb->rcv_nxt)) {
            dzlog_warn("OOO DRAIN: Discarding fully redundant entry [%u, %u) (rcv_nxt=%u).", entry->seq, entry_end,
                       tcb->rcv_nxt);
            tcb->ooo_bytes -= entry->len;
            tcb->ooo_head = entry->next;
            free(entry->data);
            free(entry);
            continue;
        }

        /* Case 2: Partial overlap — the entry starts before rcv_nxt but
         * extends past it.  Trim the already-received prefix in place so
         * the entry aligns exactly with rcv_nxt, then fall through. */
        if (SEQ_LT(entry->seq, tcb->rcv_nxt)) {
            uint32_t trim = tcb->rcv_nxt - entry->seq;
            dzlog_warn("OOO DRAIN: Trimming %u redundant bytes from entry [%u, %u) (rcv_nxt=%u).", trim, entry->seq,
                       entry_end, tcb->rcv_nxt);
            memmove(entry->data, entry->data + trim, entry->len - trim);
            tcb->ooo_bytes -= trim;
            entry->seq += trim;
            entry->len -= trim;
            /* entry->seq == rcv_nxt after trim; fall through to Case 3 */
        }

        /* Case 3: Entry starts exactly at rcv_nxt — drain it into recv_buf */
        if (entry->seq != tcb->rcv_nxt) {
            break; /* Gap still exists; nothing more to drain */
        }

        uint32_t free_space = RECV_BUF_SIZE - (tcb->recv_buf_tail - tcb->recv_buf_head);
        if (entry->len > free_space) {
            dzlog_error("OOO DRAIN: recv_buf full during drain! entry->len=%u free=%u", entry->len, free_space);
            break;
        }

        ring_buf_write(tcb->recv_buf, RECV_BUF_SIZE, tcb->recv_buf_tail, entry->data, entry->len);
        tcb->recv_buf_tail += entry->len;
        tcb->rcv_nxt += entry->len;
        tcb->ooo_bytes -= entry->len;

        tcb->ooo_head = entry->next;
        free(entry->data);
        free(entry);

        dzlog_info("OOO DRAIN: entry consumed, rcv_nxt=%u ooo_bytes=%u", tcb->rcv_nxt, tcb->ooo_bytes);
    }
}

/**
 * @brief Extract the TSecr field from the TCP Timestamp option of a received segment.
 *
 * Returns true and writes to *out_tsecr if the option is found.
 * TSecr is the echoed sender timestamp — used to compute RTT as (now - TSecr).
 */
static bool extract_tsecr(tcphdr *hdr, uint32_t *out_tsecr) {
    uint8_t hdr_bytes = (uint8_t)((hdr->th_off_flags >> 4) * 4);
    if (hdr_bytes <= (uint8_t)sizeof(tcphdr))
        return false;

    uint8_t *p   = (uint8_t *)hdr + sizeof(tcphdr);
    uint8_t *end = (uint8_t *)hdr + hdr_bytes;

    while (p < end) {
        if (*p == TCPOPT_EOL)
            break;
        if (*p == TCPOPT_NOP) {
            p++;
            continue;
        }
        if (p + 1 >= end)
            break;
        uint8_t kind = p[0];
        uint8_t len  = p[1];
        if (len < 2 || p + len > end)
            break;
        if (kind == TCPOPT_TIMESTAMP && len == TCPOLEN_TIMESTAMP) {
            /* option layout: [kind(1), len(1), TSval(4), TSecr(4)]
             * TSecr is at bytes [6..9] relative to option start */
            *out_tsecr = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16)
                       | ((uint32_t)p[8] <<  8) |  (uint32_t)p[9];
            return true;
        }
        p += len;
    }
    return false;
}

static void handle_received_data(struct tcb *tcb, tcphdr *hdr, uint8_t *data, ssize_t data_length) {
    /* Update ts_recent from the incoming segment's timestamp option.
     * This keeps ts_recent fresh so outgoing segments echo the correct TSecr. */
    process_window_option(hdr, tcb);

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

        /* RTT measurement — prefer RFC 1323 timestamp path; fall back to tick counter */
        uint32_t rtt_us_measured = 0;

        if (tcb->ts_enabled) {
            uint32_t tsecr = 0;
            if (extract_tsecr(hdr, &tsecr) && tsecr != 0) {
                uint64_t now_us = utcp_get_time_us();
                rtt_us_measured = (uint32_t)(now_us - (uint64_t)tsecr);
                dzlog_info("RTT [TS]: ack=%u tsecr=%u rtt_us=%u | "
                           "old srtt=%u ticks (%u ms) old rxtcur=%d ticks (%d ms)",
                           ack_num, tsecr, rtt_us_measured,
                           tcb->t_srtt >> 3, (tcb->t_srtt >> 3) * TCP_TICK_MS,
                           tcb->t_rxtcur, tcb->t_rxtcur * TCP_TICK_MS);
                utcp_xmit_timer(tcb, rtt_us_measured);
                tcb->t_rtt = 0;
            }
        } else if (tcb->t_rtt != 0 && SEQ_GT(ack_num, tcb->t_rtseq)) {
            /* Legacy tick-based fallback (Karn's algorithm, 10ms resolution) */
            int measured_rtt = (int)tcb->t_rtt - 1;
            rtt_us_measured  = (uint32_t)measured_rtt * TCP_TICK_MS * 1000U;
            dzlog_info("RTT: ACK %u covers tracked seq=%u | measured=%d ticks (%d ms) | "
                       "old srtt=%u ticks (%u ms) old rxtcur=%d ticks (%d ms)",
                       ack_num, tcb->t_rtseq, measured_rtt, measured_rtt * TCP_TICK_MS, tcb->t_srtt >> 3,
                       (tcb->t_srtt >> 3) * TCP_TICK_MS, tcb->t_rxtcur, tcb->t_rxtcur * TCP_TICK_MS);
            utcp_xmit_timer(tcb, rtt_us_measured);
            dzlog_info("RTT: After update | srtt=%u ticks (%u ms) rttvar=%u ticks (%u ms) "
                       "rxtcur=%d ticks (%d ms)",
                       tcb->t_srtt >> 3, (tcb->t_srtt >> 3) * TCP_TICK_MS, tcb->t_rttvar >> 2,
                       (tcb->t_rttvar >> 2) * TCP_TICK_MS, tcb->t_rxtcur, tcb->t_rxtcur * TCP_TICK_MS);
            tcb->t_rtt = 0;
        }

        // Retransmission timer management.
        if (tcb->snd_una == tcb->snd_max) {
            dzlog_info("REXMT: All data ACKed (snd_una=snd_max=%u). Disarming timer.", tcb->snd_una);
            tcb->t_timer[TCPT_REXMT] = 0;
            tcb->t_rxtshift = 0; /* All data ACKed: safe to reset backoff state (Karn's) */
        } else {
            /*
             * Data is still in-flight. Rearm the timer.
             *
             * Bug fix: if retransmission timeouts have occurred (t_rxtshift > 0),
             * Karn's algorithm prevents us from getting a fresh RTT measurement,
             * so t_rxtshift stays elevated.  We must rearm with the backed-off RTO
             * (rxtcur * backoff[rxtshift]) to avoid firing the timer far too soon
             * and triggering unnecessary retransmissions during recovery.
             */
            int rearm_ticks = tcb->t_rxtcur;
            int backoff = 1;
            if (tcb->t_rxtshift > 0) {
                backoff = tcp_backoff[tcb->t_rxtshift];
                rearm_ticks = tcb->t_rxtcur * backoff;
                if (rearm_ticks > TCPTV_REXMTMAX)
                    rearm_ticks = TCPTV_REXMTMAX;
            }
            dzlog_info("REXMT: Data still in flight (%u bytes). Restarting timer to %d ticks (%d ms) "
                       "[rxtcur=%d rxtshift=%d backoff=%d].",
                       tcb->snd_max - tcb->snd_una, rearm_ticks, rearm_ticks * TCP_TICK_MS, tcb->t_rxtcur,
                       tcb->t_rxtshift, backoff);
            tcb->t_timer[TCPT_REXMT] = rearm_ticks;
        }

        struct cc_event_args args;
        args.type = TCP_CC_EVENT_ACK;
        args.data.ack.acked_bytes = newly_acked_bytes;
        args.data.ack.rtt_us      = rtt_us_measured;

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
                 current_scaled_win <= tcb->snd_wnd && // Send window did not grow (shrink counts too)
                 tcb->snd_una != tcb->snd_max) {       // There is data in flight

            // Track shrinking window so future comparisons stay accurate
            tcb->snd_wnd = current_scaled_win;

            // Prevent overflow
            if (tcb->t_dupacks < 255) {
                tcb->t_dupacks++;
            }

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

        /* Available space must reserve room for OOO bytes that will eventually
         * drain into recv_buf, so subtract ooo_bytes from the total. */
        uint32_t free_space = RECV_BUF_SIZE - (tcb->recv_buf_tail - tcb->recv_buf_head) - tcb->ooo_bytes;
        dzlog_debug("Buffer Check: free_space=%u (ooo_bytes=%u), incoming_data=%zd", free_space, tcb->ooo_bytes,
                    data_length);

        if (data_length <= (ssize_t)free_space) { // For every byte of data, copy into ring buffer
            uint32_t old_tail = tcb->recv_buf_tail;

            ring_buf_write(tcb->recv_buf, RECV_BUF_SIZE, tcb->recv_buf_tail, data, data_length);

            tcb->recv_buf_tail += data_length;
            tcb->rcv_nxt += data_length;

            dzlog_info("IN-ORDER DATA ACCEPTED: recv_buf_tail %u -> %u, rcv_nxt %u -> %u. Waking API threads.",
                       old_tail, tcb->recv_buf_tail, (uint32_t)(tcb->rcv_nxt - data_length), tcb->rcv_nxt);

            // Wake up any thread blocking in utcp_read waiting for data
            pthread_cond_broadcast(&tcb->cond_var);

            /* Drain any OOO segments that are now consecutive with rcv_nxt.
             * This advances rcv_nxt cumulatively — a single ACK will cover
             * all the data moved out of the reassembly queue. */
            uint32_t pre_drain_rcv_nxt = tcb->rcv_nxt;
            drain_ooo_queue(tcb);

            bool gap_filled = (tcb->rcv_nxt != pre_drain_rcv_nxt);

            if (gap_filled) {
                dzlog_info("OOO DRAIN: rcv_nxt advanced %u -> %u after hole filled.", pre_drain_rcv_nxt, tcb->rcv_nxt);
                /* Wake readers again — more data is now available */
                pthread_cond_broadcast(&tcb->cond_var);

                dzlog_debug("Gap filled. Forcing immediate ACK per RFC 5681.");
                tcb->t_flags &= ~TF_DELACK;
                tcb->t_timer[TCPT_DELACK] = 0;
                tcb->t_flags |= TF_ACKNOW;
            } else if (tcb->t_dupacks > 0) {
                /* We were receiving duplicate ACKs (peer has a gap). Force immediate
                 * ACK so the sender knows we received new data without extra delay. */
                dzlog_debug("New data received while in dup-ACK state (%d dups). Forcing ACK.", tcb->t_dupacks);
                tcb->t_flags &= ~TF_DELACK;
                tcb->t_timer[TCPT_DELACK] = 0;
                tcb->t_flags |= TF_ACKNOW;
            } else if (tcb->t_flags & TF_DELACK) {
                /**
                 * RFC 1122 says send every second segment
                 */
                dzlog_debug("Second segment received. Canceling delay and ACKing.");
                tcb->t_flags &= ~TF_DELACK;
                tcb->t_timer[TCPT_DELACK] = 0;
                tcb->t_flags |= TF_ACKNOW;
            } else {
                dzlog_debug("First segment received. Starting delayed ACK timer.");
                tcb->t_flags |= TF_DELACK;
                tcb->t_timer[TCPT_DELACK] = TCPTV_DELACK;
            }

        } else {
            // Buffer overflow: Usually, you'd drop the packet or truncate
            dzlog_error("DROP (OOM): Receive buffer full! Free space %u, but tried to insert %zd bytes.", free_space,
                        data_length);
            return;
        }
    } else {
        /* seq_num > rcv_nxt: out-of-order segment.
         * Buffer it in the reassembly queue and send a duplicate ACK of
         * the last in-order byte (rcv_nxt) so the sender knows what we
         * are still waiting for.  The application cannot read past the
         * hole because recv_buf only contains contiguous in-order data. */
        dzlog_warn("OUT OF ORDER: Expected %u, got %u. Buffering and sending dup ACK.", tcb->rcv_nxt, seq_num);
        insert_ooo_segment(tcb, seq_num, data, (uint32_t)data_length);
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

    /* Use the data offset field to skip past any TCP options so that out_data
     * points at the actual application payload, not at option bytes. */
    uint8_t hdr_bytes = (uint8_t)(((*out_hdr)->th_off_flags >> 4) * 4);
    if (hdr_bytes < (uint8_t)sizeof(tcphdr))
        hdr_bytes = (uint8_t)sizeof(tcphdr);
    *out_data     = buff + hdr_bytes;
    *out_data_len = (ssize_t)buf_len - hdr_bytes;
    if (*out_data_len < 0)
        *out_data_len = 0;
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
