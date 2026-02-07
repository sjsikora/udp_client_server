#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <utcp/net/tcp.h>
#include <utcp/api.h>

void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered) {
    if (!hdr)
        return;

    uint16_t sport = net_ordered ? ntohs(hdr->th_sport) : hdr->th_sport;
    uint16_t dport = net_ordered ? ntohs(hdr->th_dport) : hdr->th_dport;
    uint32_t seq = net_ordered ? ntohl(hdr->th_seq) : hdr->th_seq;
    uint32_t ack = net_ordered ? ntohl(hdr->th_ack) : hdr->th_ack;
    uint16_t win = net_ordered ? ntohs(hdr->th_win) : hdr->th_win;

    printf("[UTCP TCP Packet]:\n"
           "  Source Port      : %u\n"
           "  Destination Port : %u\n"
           "  Sequence Number  : %u\n"
           "  Ack Number       : %u\n"
           "  Data Offset      : %u bytes\n"
           "  Flags            : [SYN=%d ACK=%d FIN=%d RST=%d PSH=%d URG=%d]\n"
           "  Window           : %u\n",
           sport, dport, seq, ack, (hdr->th_off_flags >> 4) * 4,
           (hdr->th_flags & TH_SYN) != 0, (hdr->th_flags & TH_ACK) != 0,
           (hdr->th_flags & TH_FIN) != 0, (hdr->th_flags & TH_RST) != 0,
           (hdr->th_flags & TH_PUSH) != 0, (hdr->th_flags & TH_URG) != 0, win);
}

void dump_tcb(int fd) {
    if (fd < 0 || fd >= MAX_UTCP_SOCKETS) {
        printf("dump_tcb: invalid fd %d\n", fd);
        return;
    }

    struct tcb_info *tcb = utcp_fd_table[fd];

    if (!tcb) {
        printf("dump_tcb: fd %d not in use\n", fd);
        return;
    }

    struct in_addr ip;
    ip.s_addr = tcb->id.src_ip;

    printf("==== UTCP TCB fd=%d ====\n", fd);
    printf("state      : %u\n", tcb->state);
    printf("src_ip     : %s\n", inet_ntoa(ip));
    printf("src_port   : %u\n", ntohs(tcb->id.src_port));
    printf("dst_ip     : %s\n",
           tcb->id.dst_ip ? inet_ntoa(*(struct in_addr *)&tcb->id.dst_ip)
                          : "(unset)");
    printf("dst_port   : %s\n", tcb->id.dst_port ? "set" : "(unset)");
    printf("snd_una    : %u\n", tcb->snd_una);
    printf("snd_nxt    : %u\n", tcb->snd_nxt);
    printf("rcv_nxt    : %u\n", tcb->rcv_nxt);
    printf("========================\n");
}
