#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <utcp/net/tcp.h>
#include <utcp/api.h>

void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered) {
    if (!hdr) return;

    const char *direction = net_ordered ? ">>> [OUTGOING PACKET]" : "<<< [INCOMING PACKET]";

    uint16_t sport = net_ordered ? ntohs(hdr->th_sport) : hdr->th_sport;
    uint16_t dport = net_ordered ? ntohs(hdr->th_dport) : hdr->th_dport;
    uint32_t seq   = net_ordered ? ntohl(hdr->th_seq)   : hdr->th_seq;
    uint32_t ack   = net_ordered ? ntohl(hdr->th_ack)   : hdr->th_ack;
    uint16_t win   = net_ordered ? ntohs(hdr->th_win)   : hdr->th_win;

    printf("%s\n"
           "  Source Port      : %u\n"
           "  Destination Port : %u\n"
           "  Sequence Number  : %u\n"
           "  Ack Number       : %u\n"
           "  Flags            : [ %s%s%s%s%s%s ]\n"
           "  Window           : %u\n"
           "  Header Size      : %u bytes\n"
           "----------------------------------------\n",
           direction,
           sport, dport, seq, ack,
           (hdr->th_flags & TH_SYN)  ? "SYN " : "",
           (hdr->th_flags & TH_ACK)  ? "ACK " : "",
           (hdr->th_flags & TH_FIN)  ? "FIN " : "",
           (hdr->th_flags & TH_RST)  ? "RST " : "",
           (hdr->th_flags & TH_PUSH) ? "PSH " : "",
           (hdr->th_flags & TH_URG)  ? "URG " : "",
           win,
           (hdr->th_off_flags >> 4) * 4);
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
    printf("src_ip     : %s\n", tcb->src_ip);
    printf("src_port   : %u\n", tcb->src_port);
    printf("dst_ip     : %s\n",
           tcb->dst_ip ? tcb->dst_ip
                          : "(unset)");
    printf("dst_port   : %s\n", tcb->dst_port ? "set" : "(unset)");
    printf("snd_una    : %u\n", tcb->snd_una);
    printf("snd_nxt    : %u\n", tcb->snd_nxt);
    printf("rcv_nxt    : %u\n", tcb->rcv_nxt);
    printf("========================\n");
}
