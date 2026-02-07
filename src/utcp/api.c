/**
 * Defines the UTCP API. The UTCP (TCP-over-UDP) is a user made construct of
 * TCP. A UTCP socket will follow the RFC guidelines for TCP, and the only
 * note is that any UTCP traffic will go through a predestinted single UDP
 * port.
 *
 *                        ┌─────────────────────────┐
 *    Application A  ───▶ │ UTCP socket (port 10000)│
 *    Application B  ───▶ │ UTCP socket (port 10001)│
 *    Application C  ───▶ │ UTCP socket (port 443)  │
 *                        └─────────────┬───────────┘
 *                                      │
 *                              user-space demux
 *                                      │
 *                              ONE real UDP socket
 *                             bound to port UDP_PORT
 *                                      │
 *                                    kernel
 *
 * The API of the UTCP tries to mimic the berckly sockets API as best as
 * possible. So, from an application view, you could swap out bind() from BS
 * with utcp_bind() with no issue.
 */

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

int UDP_PORT = -1; // Host order global UDP port
static int utcp_initialized = 0;
static int udp_fd = -1;
struct tcb_info *utcp_fd_table[MAX_UTCP_SOCKETS] = {0};

static void deserialize_utcp_packet(uint8_t *buff, size_t buf_len,
                                    tcphdr **out_hdr, uint8_t **out_data,
                                    ssize_t *out_data_len) {

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

static ssize_t Recvfrom(void *buf, size_t len, int flags,
                        struct sockaddr *__restrict src_addr,
                        socklen_t *__restrict addrlen) {
    ssize_t data = recvfrom(udp_fd, buf, len, flags, src_addr, addrlen);

    // Cast data recieved to TCP header
    if (data < 0)
        err_sys("UTCP recvfrom failed");

    return data;
}

void utcp_package_init(int local_udp_port) {
    if (utcp_initialized)
        return;

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,                     // Listen on IPv4
        .sin_port = htons(local_udp_port),         // Listen on port UDP_PORT
        .sin_addr.s_addr = inet_addr("127.0.0.1"), // Listen on localhost
    };

    if ((udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
        err_sys("socketerror");
    if (bind(udp_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
        err_sys("Bind failed for local UDP port");

    struct sockaddr_in bound_addr;
    socklen_t addrlen = sizeof(bound_addr);

    if (getsockname(udp_fd, (struct sockaddr *)&bound_addr, &addrlen) < 0)
        err_sys("getsockname failed");

    UDP_PORT = ntohs(bound_addr.sin_port); // Update with the actual port
    printf("[UTCP] UTCP package is initlized and is listening on port %u\n",
           UDP_PORT);

    utcp_initialized = 1;
}

static struct tcb_info *utcp_get_tcb(int fd) {
    /**
     * @brief Retieve a utcp tcb by fd
     *
     * Validates the file descriptor and tcb. Errors if
     * fd is invalid (such as -1) or tcb was never allocated
     * for the fd
     */

    if (fd < 0 || fd >= MAX_UTCP_SOCKETS)
        err_sys("Invalid UTCP fd");

    struct tcb_info *tcb = utcp_fd_table[fd];
    if (!tcb)
        err_sys("UTCP fd not allocated");

    return tcb;
}

static struct tcb_info *utcp_get_tcb_in_state(int fd, enum tcp_state required) {
    /**
     * @brief Retieve a utcp tcb by fd and verify it is in a state
     */
    struct tcb_info *tcb = utcp_get_tcb(fd);

    if (tcb->state != required)
        err_sys("UTCP socket in invalid state");

    return tcb;
}

int utcp_socket(void) {
    utcp_package_init(1970);

    // Loop the table for utcp file descriptors and find a available one
    int utcp_fd;
    for (utcp_fd = 0; utcp_fd < MAX_UTCP_SOCKETS; utcp_fd++) {
        if (utcp_fd_table[utcp_fd] == NULL)
            break;
    }

    if (utcp_fd == MAX_UTCP_SOCKETS)
        err_sys("Too many UTCP sockets open");

    // Allocate a new TCB for this fd
    struct tcb_info *tcb = calloc(1, sizeof(struct tcb_info));
    if (!tcb)
        err_sys("calloc failed for TCB");

    tcb->state = TCP_CLOSE;

    // Init acknowledgment numbers
    tcb->iss = 0x0000;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;

    utcp_fd_table[utcp_fd] = tcb;
    return utcp_fd;
}

static int utcp_send(int fd, const void *buf, size_t len, int flags) {
    /**
     * @brief Send buffer in the fd's UTCP socket
     *
     * The defacto send over UTCP function. It works by creating a
     * TCP segement, adding in the apporiate details and sending the
     * packet off in the global UDP port.
     *
     */

    struct tcb_info *tcb = utcp_get_tcb(fd);

    // Reconstruct sockaddr_in from the tcb (possible optimization)
    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(tcb->dst_udp_port);
    dst_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Allocate memory for segment (header + data)
    size_t segment_size = sizeof(tcphdr) + len;
    struct tcp_segment *seg = malloc(segment_size);
    if (!seg)
        err_sys("malloc failed for TCP segment");

    // Fill TCP header
    memset(&seg->hdr, 0, sizeof(tcphdr));
    seg->hdr.th_sport = htons(tcb->id.src_port);
    seg->hdr.th_dport = htons(tcb->id.dst_port);
    seg->hdr.th_seq = htonl(tcb->snd_nxt); // convert to network order
    seg->hdr.th_ack = htonl(tcb->rcv_nxt); // Last recived for now
    seg->hdr.th_off_flags = (sizeof(tcphdr) / 4)
                            << 4; // Convert into 32-bit words
    seg->hdr.th_flags = flags;
    seg->hdr.th_win = htons(1024); // dummy window

    // Copy the buffer into the segment
    memcpy(seg->data, buf, len);

    printf("utcp_send: sending to true UDP port %u, UTCP port %u\n",
           tcb->dst_udp_port, tcb->id.dst_port);

    debug_print_tcp_packet(&seg->hdr, true);

    ssize_t sent_bytes = sendto(udp_fd, seg, segment_size, 0,
                                (struct sockaddr *)&dst_addr, sizeof(dst_addr));

    if (sent_bytes < 0)
        err_sys("UTCP sendto failed");

    free(seg);

    tcb->snd_nxt += len;

    return len;
}

int utcp_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    if (tcb->state != TCP_CLOSE)
        err_sys("TCB is in a invalid state for binding");
    if (tcb->id.src_ip != 0)
        err_sys("UTCP socket is already bound");
    if (sin->sin_family != AF_INET)
        err_sys("No support for UTCP ports that are not AF_INET");

    // TODO: Validate port

    // Assume we were given the port in network order TCB holds in host order
    tcb->id.src_ip = (uint32_t)ntohl(sin->sin_addr.s_addr);
    tcb->id.src_port = (uint16_t)ntohs(sin->sin_port);

    return 0;
}

int utcp_syn(int fd) {
    /**
     * @brief Send SYN segment to the destination
     */

    printf("[UTCP Client] Sending SYN request");

    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);

    if (tcb->id.dst_port == 0 || tcb->id.dst_ip == 0)
        err_sys("Destination IP/Port must be set before SYN");

    utcp_send(fd, NULL, 0, TH_SYN);

    tcb->state = TCP_SYN_SENT;
    tcb->snd_nxt += 1;

    return 0;
}

int utcp_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    if (sin->sin_family != AF_INET)
        err_sys("Only AF_INET supported for UTCP connect");

    tcb->id.dst_ip = sin->sin_addr.s_addr;
    tcb->id.dst_port = ntohs(sin->sin_port);
    tcb->dst_udp_port = 1970; // UTCP server

    // Init sequence numbers:
    tcb->snd_una = 0;
    tcb->snd_nxt = 0;
    tcb->rcv_nxt = 0;

    utcp_syn(fd);

    socklen_t fromlen;
    struct sockaddr_in from;
    fromlen = sizeof(from);

    uint8_t *buff = malloc(1500);
    ssize_t buff_length = 1500;

    printf("[UTCP Client] Waiting for SYN-ACK response\n");
    ssize_t packet_length =
        Recvfrom(buff, buff_length, 0, (struct sockaddr *)&from, &fromlen);

    tcphdr *hdr;
    uint8_t *data;
    ssize_t data_length;

    deserialize_utcp_packet(buff, packet_length, &hdr, &data, &data_length);

    tcb->rcv_nxt = ntohl(hdr->th_seq) + 1;

    printf("[UTCP Client] SYN-ACK Packet from server:\n");
    debug_print_tcp_packet(hdr, false);

    if (!(hdr->th_flags == (TH_SYN | TH_ACK)))
        err_sys("Server did not SYN-ACK");

    printf("[UTCP Client] Sending ACK\n");
    utcp_send(fd, NULL, 0, TH_ACK);
    tcb->rcv_nxt = ntohl(hdr->th_seq) + 1;

    printf("[UTCP Client] Handshake finished.\n");
    free(buff);

    return 0;
}

static int utcp_find_tcb(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                         uint16_t dst_port) {
    /**
     * @brief Finds file descriptor associated with tcp 4-tuple
     *
     */

    for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
        struct tcb_info *tcb = utcp_fd_table[i];
        if (!tcb)
            continue;

        if (tcb->id.src_ip == dst_ip && tcb->id.src_port == dst_port &&
            tcb->id.dst_ip == src_ip && tcb->id.dst_port == src_port) {
            return i;
        }
    }

    err_sys("Can not find fd with this tuple");
    return -1;
}

int utcp_listen_for_syn(int fd) {
    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);
    tcb->state = TCP_LISTEN;

    // Allocate room to see sender
    socklen_t fromlen;
    struct sockaddr_in from;
    fromlen = sizeof(from);

    // Allocate room for buffer
    uint8_t *buff = malloc(1500);
    ssize_t buff_len = 1500;

    printf("[UTCP Server] Waiting for SYN\n");
    ssize_t packet_size =
        Recvfrom(buff, buff_len, 0, (struct sockaddr *)&from, &fromlen);

    tcb->state = TCP_SYN_RECV;

    tcphdr *hdr;
    uint8_t *data;
    ssize_t data_len;

    deserialize_utcp_packet(buff, packet_size, &hdr, &data, &data_len);

    printf("[UTCP Server] Received SYN packet:\n");
    debug_print_tcp_packet(hdr, false);

    if (!(hdr->th_dport == tcb->id.src_port)) {
        fprintf(stderr,
                "[UTCP DEBUG] Received packet for port %u, but expected port "
                "%u (fd mismatch)\n",
                hdr->th_dport, tcb->id.src_port);
        err_sys("Received packet for a different port than fd");
    }
    if (!(hdr->th_flags & TH_SYN))
        err_sys("Packet recieved, but no it did not have a SYN");

    tcb->id.dst_port = hdr->th_sport;
    tcb->id.dst_ip = ntohl(from.sin_addr.s_addr);
    tcb->dst_udp_port = ntohs(from.sin_port);
    tcb->irs = ntohs(hdr->th_seq);
    tcb->rcv_nxt = tcb->irs + 1; // Increase sequence number by one

    printf("[UTCP Server] Sending SYN-ACK:\n");

    // Sending SYN-ACK
    utcp_send(fd, NULL, 0, TH_SYN | TH_ACK);

    tcb->snd_nxt += 1;

    printf("[UTCP Server] Waiting for ACK\n");
    packet_size =
        Recvfrom(buff, buff_len, 0, (struct sockaddr *)&from, &fromlen);
    deserialize_utcp_packet(buff, packet_size, &hdr, &data, &data_len);

    printf("[UTCP Server] Recieved ACK packet:\n");
    debug_print_tcp_packet(hdr, false);

    if (!(hdr->th_flags & TH_SYN))

        print_safe_chars(data, data_len);

    free(buff);

    return 0;
}
