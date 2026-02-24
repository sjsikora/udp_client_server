#include "utils.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <utcp/api.h>
#include <utcp/net/tcp.h>

void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered, const uint8_t *payload, size_t payload_len) {
    if (!hdr)
        return;

    const char *direction = net_ordered ? ">>> [OUTGOING PACKET]" : "<<< [INCOMING PACKET]";

    uint16_t sport = net_ordered ? ntohs(hdr->th_sport) : hdr->th_sport;
    uint16_t dport = net_ordered ? ntohs(hdr->th_dport) : hdr->th_dport;
    uint32_t seq = net_ordered ? ntohl(hdr->th_seq) : hdr->th_seq;
    uint32_t ack = net_ordered ? ntohl(hdr->th_ack) : hdr->th_ack;
    uint16_t win = net_ordered ? ntohs(hdr->th_win) : hdr->th_win;

    printf("%s\n"
           "  Source Port      : %u\n"
           "  Destination Port : %u\n"
           "  Sequence Number  : %u\n"
           "  Ack Number       : %u\n"
           "  Flags            : [ %s%s%s%s%s%s ]\n"
           "  Window           : %u\n",
           direction, sport, dport, seq, ack, (hdr->th_flags & TH_SYN) ? "SYN " : "",
           (hdr->th_flags & TH_ACK) ? "ACK " : "", (hdr->th_flags & TH_FIN) ? "FIN " : "",
           (hdr->th_flags & TH_RST) ? "RST " : "", (hdr->th_flags & TH_PUSH) ? "PSH " : "",
           (hdr->th_flags & TH_URG) ? "URG " : "", win);

    // Only print payload info if a length is provided
    if (payload_len > 0) {
        printf("  Payload Length   : %zu bytes\n", payload_len);

        if (payload != NULL) {
            printf("  Payload Data     : ");
            size_t display_len = (payload_len > 64) ? 64 : payload_len; // Limit characters to 64
            print_safe_chars(payload, display_len);

            if (payload_len > 64) {
                printf("                     [... truncated ...]\n");
            }
        }
    }

    printf("----------------------------------------\n");
}

void dump_tcb(int fd) {
    if (fd < 0 || fd >= MAX_UTCP_SOCKETS) {
        printf("dump_tcb: invalid fd %d\n", fd);
        return;
    }

    struct tcb *tcb = utcp_fd_table[fd];

    if (!tcb) {
        printf("dump_tcb: fd %d not in use\n", fd);
        return;
    }

    struct in_addr ip;
    ip.s_addr = tcb->src_ip;

    printf("==== UTCP TCB fd=%d ====\n", fd);
    printf("state      : %u\n", tcb->state);
    printf("src_ip     : %u\n", tcb->src_ip);
    printf("src_port   : %u\n", tcb->src_port);
    printf("dst_ip     : %s\n", tcb->dst_ip ? tcb->dst_ip : "(unset)");
    printf("dst_port   : %s\n", tcb->dst_port ? "set" : "(unset)");
    printf("snd_una    : %u\n", tcb->snd_una);
    printf("snd_nxt    : %u\n", tcb->snd_nxt);
    printf("rcv_nxt    : %u\n", tcb->rcv_nxt);
    printf("========================\n");
}
