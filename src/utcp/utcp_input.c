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

        // Find coorsponding TCB
        struct tcb *tcb = find_tcb(hdr, ntohl(from.sin_addr.s_addr));

        if(tcb == NULL) err_sys("UTCP packet came with no active socket");

        switch (tcb->state) {
            case TCP_LISTEN: // If SYN flag set, accept new conneciton
                if(hdr->th_flags & TH_SYN) {
                    printf("Received SYN from %u:%d\n", ntohl(from.sin_addr.s_addr), hdr->th_sport);


                    tcb->dst_port = hdr->th_sport;
                    tcb->dst_ip = ntohl(from.sin_addr.s_addr);
                    tcb->dst_udp_port = ntohs(from.sin_port);
                    tcb->irs = ntohl(hdr->th_seq);
                    tcb->rcv_nxt = tcb->irs + 1; // Increase sequence number by one

                    // TODO: Send a packet to SYN-ACK

                    tcb->state = TCP_SYN_RECV;


                }
                break;

            case TCP_SYN_SENT: // If ACK of our SYN, connection is completed!
                if ((hdr->th_flags & TH_SYN) && (hdr->th_flags & TH_ACK)) {
                    if (hdr->th_ack == tcb->snd_nxt + 1) {
                        printf("Connection Established with UTCP server\n");
                        tcb->rcv_nxt = hdr->th_seq + 1;
                        tcb->snd_nxt = hdr->th_ack;

                        // TODO: Send the final ACK utcp_send_packet(tcb, TH_ACK);

                        tcb->state = TCP_ESTABLISHED;
                    }
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
        tcb->send_buf_head = (tcb->send_buf_head + newly_acked_bytes) % SEND_BUF_SIZE; //
        tcb->snd_wnd = hdr->th_win;
    } else {
        printf("Packet was sent acking invalid bytes.");
    }

    /* Recieve window: Handle my acknowledgment */
    uint32_t seq_num = hdr->th_seq;
    uint32_t data_len = data_length;


    if (seq_num == tcb->rcv_nxt) { // Is this the packet we are expecting?

        // ...

    } else if (SEQ_LT(seq_num, tcb->rcv_nxt)) {
        /**
         * Retransmission of old data. We already have this data in our
         * recieve buffer all correct. The system just hasn't recieved our
         * ACK for the data.
         */
    }





    if (data_length <= 0) return;

    // Check sequence and ack numbers


    // Trim any data needed

    // Place data in the tcb buffer
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
static struct tcb* find_tcb(struct tcphdr *hdr, uint32_t src_ip) {
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
