#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utcp/net/tcp.h>
#include <utcp/api.h>
#include <utcp/utcp_utils.h>
#include <utils.h>
#include <utcp/utcp_init.h>
#include <utcp/utcp_output.h>

static void handle_received_data(struct tcb*, tcphdr*, uint8_t*, ssize_t);
static ssize_t Recvfrom(void*, size_t, int, struct sockaddr* __restrict, socklen_t* __restrict);
static void deserialize_utcp_packet(uint8_t*, size_t, tcphdr**, uint8_t**, ssize_t*);
static struct tcb* find_tcb(tcphdr*, uint32_t);

int utcp_input(struct tcb *tcb) {
    // Allocate variables we will reuse for every incoming segment
    socklen_t fromlen;
    struct sockaddr_in from;
    fromlen = sizeof(from);

    uint8_t *buff = malloc(1500);
    ssize_t buff_len = 1500;

    tcphdr *hdr;
    uint8_t *data;
    ssize_t data_length;


    for(;;) {
        // Wait for incoming packet and deserialize
        ssize_t packet_length = Recvfrom(buff, buff_len, 0, (struct sockaddr *)&from, &fromlen);
        deserialize_utcp_packet(buff, packet_length, &hdr, &data, &data_length);
        debug_print_tcp_packet(hdr, false, data, data_length);

        // Find coorsponding TCB
        struct tcb *tcb = find_tcb(hdr, ntohl(from.sin_addr.s_addr));
        PRINT_TCP_VARS(tcb, "INPUT PRE-PROC");

        if(tcb == NULL) err_sys("UTCP packet came with no active socket");

        switch (tcb->state) {
            case TCP_LISTEN: // If SYN flag set, accept new conneciton
                if(hdr->th_flags & TH_SYN) {
                    printf("Received SYN from %u:%d\n", ntohl(from.sin_addr.s_addr), hdr->th_sport);

                    tcb->dst_port = hdr->th_sport;
                    tcb->dst_ip = ntohl(from.sin_addr.s_addr);
                    tcb->dst_udp_port = ntohs(from.sin_port);
                    tcb->irs = hdr->th_seq;
                    tcb->rcv_nxt = tcb->irs + 1; // Increase sequence number by one
                    tcb->snd_wnd = hdr->th_win;

                    tcb->iss = 0;
                    tcb->snd_nxt = tcb->iss;
                    tcb->snd_una = tcb->iss;
                    tcb->state = TCP_SYN_RECV;

                    utcp_output(tcb);
                }
                break;

            case TCP_SYN_SENT:
                if ((hdr->th_flags & TH_SYN) && (hdr->th_flags & TH_ACK)) { // SYN-ACK Packet
                    if (hdr->th_ack == tcb->snd_nxt) {

                        tcb->snd_una = hdr->th_ack;
                        tcb->irs = hdr->th_seq; // Set the server's inital recieve sequence
                        tcb->rcv_nxt = hdr->th_seq + 1; // We are now ready to recieve the (irs [or SYN bit] + 1 ) byte
                        tcb->snd_wnd = hdr->th_win;
                        tcb->state = TCP_ESTABLISHED;
                        utcp_output(tcb);

                        printf("Connection Established with UTCP server\n");
                    }
                }
                break;

            case TCP_SYN_RECV:
                if ((hdr->th_flags & TH_ACK) && (hdr->th_ack == tcb->snd_nxt)) { // The final ACK of the 3-way handshake
                    tcb->state = TCP_ESTABLISHED;
                    tcb->snd_una = hdr->th_ack;
                    printf("Handshake complete (Server side)\n");
                    break;
                }
            // Fall through to TCP_ESTABLISHED to handle the data in the same segment
            case TCP_ESTABLISHED:
                handle_received_data(tcb, hdr, data, data_length);
                break;
        }

    }
    // Unreachable code
    free(buff);
}

static void handle_received_data(
    struct tcb *tcb,
    tcphdr *hdr,
    uint8_t *data,
    ssize_t data_length
) {
    /* Handle Acknowledgement */
    uint32_t ack_num = hdr->th_ack;

    if (
        SEQ_GT(ack_num, tcb->snd_una) && // Ensure packet isn't ACKing bytes that were already ACKed
        SEQ_LEQ(ack_num, tcb->snd_nxt) // Ensure packet isn't ACKing unsent butes
    ) {
        uint32_t newly_acked_bytes = ack_num - tcb->snd_una;

        // Update new oldest unacked number
        tcb->snd_una = ack_num;

        // Slide the window over
        tcb->send_buf_head = tcb->send_buf_head + newly_acked_bytes;
        tcb->snd_wnd = hdr->th_win;
    } else {
        printf("Duplicate ACK\n");
    }

    /* Recieve window: Handle my acknowledgment */
    if (data_length <= 0) return;

    uint32_t seq_num = hdr->th_seq;

    if (seq_num == tcb->rcv_nxt) { // Is this the packet we are expecting?

        // See how much room we have left in the buffer
        uint32_t free_space = RECV_BUF_SIZE - (tcb->recv_buf_tail - tcb->recv_buf_head);

        if (data_length <= (ssize_t)free_space) { // For every byte of data, copy into ring buffer
            for (ssize_t i = 0; i < data_length; i++) {
                tcb->recv_buf[(tcb->recv_buf_tail + i) % RECV_BUF_SIZE] = data[i];
            }

            tcb->recv_buf_tail += data_length;
            tcb->rcv_nxt += data_length;

        } else {
            // Buffer overflow: Usually, you'd drop the packet or truncate
            printf("Receive buffer full, dropping data.\n");
            return;
        }
    } else if (SEQ_LT(seq_num, tcb->rcv_nxt)) {
        /**
         * Retransmission of old data. We already have this data in our
         * recieve buffer all correct. The system just hasn't recieved our
         * ACK for the data.
         */
        printf("Received duplicate data (retransmission). Re-acking.\n");
    } else {
        /**
         * Out-of-order data. Simply just drop this packet
         */
        printf("Received out-of-order packet. Expected %u, got %u\n", tcb->rcv_nxt, seq_num);
        return;
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
static void deserialize_utcp_packet(
    uint8_t *buff,
    size_t buf_len,
    tcphdr **out_hdr,
    uint8_t **out_data,
    ssize_t *out_data_len
) {

    if (buf_len < sizeof(tcphdr))
        err_sys(
            "Can not parse utcp packet. Are you sure this was sent correctly?");

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
static struct tcb* find_tcb(tcphdr *hdr, uint32_t src_ip) {
    struct tcb *listen_match = NULL;

    for(int i = 0; i < MAX_UTCP_SOCKETS; i++) {
        struct tcb *tcb_found = utcp_fd_table[i];
        if (!tcb_found) continue;

        if (tcb_found->src_port == hdr->th_dport &&
            tcb_found->dst_port == hdr->th_sport &&
            tcb_found->dst_ip == src_ip) {
            return tcb_found;
        }

        if (tcb_found->state == TCP_LISTEN && tcb_found->src_port == hdr->th_dport) {
            listen_match = tcb_found;
        }
    }

    return listen_match;
}

static ssize_t Recvfrom(void *buf, size_t len, int flags,
                        struct sockaddr *__restrict src_addr,
                        socklen_t *__restrict addrlen) {
    ssize_t data = recvfrom(udp_fd, buf, len, flags, src_addr, addrlen);

    // Cast data recieved to TCP header
    if (data < 0) err_sys("UTCP recvfrom failed");

    return data;
}
