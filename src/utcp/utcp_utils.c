#include "utils.h"
#include "zlog.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <utcp/api.h>
#include <utcp/net/tcp.h>

uint64_t utcp_get_time_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        err_sys("clock_gettime failed");
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

void ring_buf_read(const uint8_t *ring_buf, uint32_t buf_size, uint32_t offset, uint8_t *dst, size_t len) {
    if (len == 0)
        return;

    uint32_t physical_offset = offset % buf_size;

    if (physical_offset + len <= buf_size) {
        // Contiguous read
        memcpy(dst, &ring_buf[physical_offset], len);
    } else {
        // Wrap-around read
        size_t part1_len = buf_size - physical_offset;
        size_t part2_len = len - part1_len;

        memcpy(dst, &ring_buf[physical_offset], part1_len);
        memcpy(dst + part1_len, &ring_buf[0], part2_len);
    }
}

void ring_buf_write(uint8_t *ring_buf, uint32_t buf_size, uint32_t offset, const uint8_t *src, size_t len) {
    if (len == 0)
        return;

    uint32_t physical_offset = offset % buf_size;

    if (physical_offset + len <= buf_size) {
        // Contiguous write
        memcpy(&ring_buf[physical_offset], src, len);
    } else {
        // Wrap-around write
        size_t part1_len = buf_size - physical_offset;
        size_t part2_len = len - part1_len;

        memcpy(&ring_buf[physical_offset], src, part1_len);
        memcpy(&ring_buf[0], src + part1_len, part2_len);
    }
}

void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered, const uint8_t *payload, size_t payload_len) {
    if (!hdr)
        return;

    const char *direction = net_ordered ? ">>> [OUTGOING PACKET]" : "<<< [INCOMING PACKET]";

    uint16_t sport = net_ordered ? ntohs(hdr->th_sport) : hdr->th_sport;
    uint16_t dport = net_ordered ? ntohs(hdr->th_dport) : hdr->th_dport;
    uint32_t seq = net_ordered ? ntohl(hdr->th_seq) : hdr->th_seq;
    uint32_t ack = net_ordered ? ntohl(hdr->th_ack) : hdr->th_ack;
    uint16_t win = net_ordered ? ntohs(hdr->th_win) : hdr->th_win;

    char payload_buf[128] = "";
    if (payload_len > 0 && payload != NULL) {
        size_t display_len = (payload_len > 64) ? 64 : payload_len;
        size_t i;

        // FIXED: Pass the unsigned uint8_t directly to isprint to avoid negative char UB
        for (i = 0; i < display_len; i++) {
            payload_buf[i] = isprint(payload[i]) ? (char)payload[i] : '.';
        }

        // Append the truncation message if it was too long
        if (payload_len > 64) {
            snprintf(payload_buf + i, sizeof(payload_buf) - i, " [... truncated ...]");
        } else {
            payload_buf[i] = '\0'; // Ensure it's null-terminated
        }
    }

    // Log everything in one atomic chunk so threads don't scramble the output
    if (payload_len > 0) {
        dzlog_debug("\n"
                    "%s\n"
                    "  Source Port      : %u\n"
                    "  Destination Port : %u\n"
                    "  Sequence Number  : %u\n"
                    "  Ack Number       : %u\n"
                    "  Flags            : [ %s%s%s%s%s%s ]\n"
                    "  Window           : %u\n"
                    "  Payload Length   : %zu bytes\n"
                    "  Payload Data     : %s\n"
                    "----------------------------------------",
                    direction, sport, dport, seq, ack, (hdr->th_flags & TH_SYN) ? "SYN " : "",
                    (hdr->th_flags & TH_ACK) ? "ACK " : "", (hdr->th_flags & TH_FIN) ? "FIN " : "",
                    (hdr->th_flags & TH_RST) ? "RST " : "", (hdr->th_flags & TH_PUSH) ? "PSH " : "",
                    (hdr->th_flags & TH_URG) ? "URG " : "", win, payload_len, payload_buf);
    } else {
        dzlog_debug("\n"
                    "%s\n"
                    "  Source Port      : %u\n"
                    "  Destination Port : %u\n"
                    "  Sequence Number  : %u\n"
                    "  Ack Number       : %u\n"
                    "  Flags            : [ %s%s%s%s%s%s ]\n"
                    "  Window           : %u\n"
                    "----------------------------------------",
                    direction, sport, dport, seq, ack, (hdr->th_flags & TH_SYN) ? "SYN " : "",
                    (hdr->th_flags & TH_ACK) ? "ACK " : "", (hdr->th_flags & TH_FIN) ? "FIN " : "",
                    (hdr->th_flags & TH_RST) ? "RST " : "", (hdr->th_flags & TH_PUSH) ? "PSH " : "",
                    (hdr->th_flags & TH_URG) ? "URG " : "", win);
    }
}

void dump_tcb(int fd) {
    if (fd < 0 || fd >= MAX_UTCP_SOCKETS) {
        dzlog_error("dump_tcb: invalid fd %d", fd);
        return;
    }

    struct tcb *tcb = utcp_fd_table[fd];

    if (!tcb) {
        dzlog_error("dump_tcb: fd %d not in use", fd);
        return;
    }

    // Helper formatting to convert uint32_t IPs into standard string format
    struct in_addr src_addr, dst_addr;
    src_addr.s_addr = htonl(tcb->src_ip);
    dst_addr.s_addr = htonl(tcb->dst_ip);

    dzlog_debug("\n"
                "==== UTCP TCB fd=%d ====\n"
                "state      : %u\n"
                "src_ip     : %s\n"
                "src_port   : %u\n"
                "dst_ip     : %s\n"
                "dst_port   : %u\n"
                "snd_una    : %u\n"
                "snd_nxt    : %u\n"
                "rcv_nxt    : %u\n"
                "========================",
                fd, tcb->state, inet_ntoa(src_addr), tcb->src_port, tcb->dst_ip ? inet_ntoa(dst_addr) : "(unset)",
                tcb->dst_port, tcb->snd_una, tcb->snd_nxt, tcb->rcv_nxt);
}
