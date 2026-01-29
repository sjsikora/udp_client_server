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
 * The API of the UTCP tries to mimic the berckly sockets API as best as possible.
 * So, from an application view, you could swap out bind() from BS with utcp_bind()
 * with no issue.
*/


#include <udp_client_server/net/utcp_api.h>
#include <string.h>
#include <udp_client_server/net/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define MAX_UTCP_SOCKETS 6
#define UDP_PORT 1970

struct tcb_info *utcp_fd_table[MAX_UTCP_SOCKETS] = {0};
static int utcp_initialized = 0;
static int udp_fd = -1;

static void err_sys(const char* x)
{
    perror(x);
    exit(1);
}

void dump_tcb(int fd)
{
    /**
     * @brief Helper function to print out the state of a tcb
     *
    */

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
    printf("dst_port   : %s\n",
           tcb->id.dst_port ? "set" : "(unset)");
    printf("snd_una    : %u\n", tcb->snd_una);
    printf("snd_nxt    : %u\n", tcb->snd_nxt);
    printf("rcv_nxt    : %u\n", tcb->rcv_nxt);
    printf("========================\n");
}

static void utcp_package_init(void)
{
    /**
     * @brief Initializes the utcp (TCP-over-UDP) package
     *
     * This function will allocate a UDP socket, and bind information
     * to that port. This will allow traffic to follow through the
     * UDP and into our UTCP sockets.
     *
    */

    if (utcp_initialized) return;

    const struct sockaddr_in addr = {
        .sin_family = AF_INET, // Listen on IPv4
        .sin_port   = htons(UDP_PORT), // Listen on port UDP_PORT
        .sin_addr.s_addr = htonl(INADDR_ANY), // Listen on every IP (localhost, network ip, etc)
    };

    if ((udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) err_sys("socketerror");
    if (bind(udp_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) err_sys("Bind failed for UDP port 1970");

    utcp_initialized = 1;
}

static struct tcb_info *utcp_get_tcb(int fd)
{
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

static struct tcb_info *utcp_get_tcb_in_state(int fd, enum tcp_state required)
{
    /**
     * @brief Retieve a utcp tcb by fd and verify it is in a state
     */
    struct tcb_info *tcb = utcp_get_tcb(fd);

    if (tcb->state != required) err_sys("UTCP socket in invalid state");

    return tcb;
}

int utcp_socket(void)
{
    /**
     * @brief Creates a UTCP socket.
     *
     * This function will create a utcp socket by initilizating a tcb
     * for the session.
     *
     * @return int A utcp file descriptor that references the socket.
     * @note Exits the program if underlying UDP socket creation fails
    */

    utcp_package_init();

    // Loop the table for utcp file descriptors and find a available one
    int utcp_fd;
    for (utcp_fd = 0; utcp_fd < MAX_UTCP_SOCKETS; utcp_fd++)
    {
        if (utcp_fd_table[utcp_fd] == NULL) break;
    }

    if (utcp_fd == MAX_UTCP_SOCKETS) err_sys("Too many UTCP sockets open");

    // Allocate a new TCB for this fd
    struct tcb_info *tcb = calloc(1, sizeof(struct tcb_info));
    if (!tcb) err_sys("calloc failed for TCB");

    tcb->state = TCP_CLOSE;

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
    dst_addr.sin_port = tcb->id.dst_port;
    dst_addr.sin_addr.s_addr = tcb->id.dst_ip;

    // Allocate memory for segment (header + data)
    size_t segment_size = sizeof(tcphdr) + len;
    struct tcp_segment *seg = malloc(segment_size);
    if (!seg) err_sys("malloc failed for TCP segment");


    // Fill TCP header
    memset(&seg->hdr, 0, sizeof(tcphdr));
    seg->hdr.th_sport = tcb->id.src_port;
    seg->hdr.th_dport = tcb->id.dst_port;
    seg->hdr.th_seq   = htonl(tcb->snd_nxt);  // convert to network order
    seg->hdr.th_ack   = htonl(tcb->rcv_nxt);  // Last recived for now
    seg->hdr.th_off_flags = (sizeof(tcphdr)/4) << 4; // Convert into 32-bit words
    seg->hdr.th_flags = flags;
    seg->hdr.th_win = htons(1024);  // dummy window

    // Copy the buffer into the segment
    memcpy(seg->data, buf, len);

    ssize_t sent_bytes = sendto(
        udp_fd,
        seg,
        segment_size,
        0,
        (struct sockaddr *)&dst_addr,
        sizeof(dst_addr)
    );

    if (sent_bytes < 0)
        err_sys("UTCP sendto failed");

    free(seg);

    tcb->snd_nxt += len;

    return len;
}


int utcp_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    /**
     * @brief Binds a sockaddr to a UTCP port
     *
     * Populates the LOCAL side of side of a UTCP socket. Notice, there are
     * some special things happening here. The port that is given by the sockaddr
     * is not a port that you would see anywhere else. Rather, it is a UTCP, our own
     * special port that we track.
     *
     */

    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);
    const struct sockaddr_in *sin = (const struct sockaddr_in *) addr;

    if (tcb -> state != TCP_CLOSE) err_sys("TCB is in a invalid state for binding");
    if (tcb -> id.src_ip != 0) err_sys("UTCP socket is already bound");
    if(sin->sin_family != AF_INET) err_sys("No support for UTCP ports that are not AF_INET");

    // TODO: Validate port

    uint16_t port = ntohs(sin->sin_port);
    uint32_t ip = sin->sin_addr.s_addr; // Network order

    tcb->id.src_ip   = ip;
    tcb->id.src_port = htons(port);

    return 0;
}


int utcp_syn(int fd) {
    /**
     * @brief Send SYN segment to the destination
     *
    */

    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);

    if (tcb->id.dst_port == 0 || tcb->id.dst_ip == 0) err_sys("Destination IP/Port must be set before SYN");

    // Init acknowledgment numbers
    tcb->iss = 0x0000;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;

    utcp_send(fd, NULL, 0, TH_SYN);

    tcb->state = TCP_SYN_SENT;
    tcb->snd_nxt += 1;

    return 0;
}


int utcp_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    /**
     * @brief Connect to another UTCP socket in addr
     *
     * Initlizes the three-way handshake.
    */

    struct tcb_info *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSE);
    const struct sockaddr_in *sin = (const struct sockaddr_in *) addr;

    if (sin->sin_family != AF_INET) err_sys("Only AF_INET supported for UTCP connect");

    tcb->id.dst_ip = sin->sin_addr.s_addr;
    tcb->id.dst_port = sin->sin_port;

    // Init sequence numbers:
    tcb->snd_una = 0;
    tcb->snd_nxt = 0;
    tcb->rcv_nxt = 0;

    utcp_syn(fd);
}

static int utcp_find_tcb(uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port) {
    /**
     * @brief Finds file descriptor associated with tcp 4-tuple
     *
    */

    for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
        struct tcb_info *tcb = utcp_fd_table[i];
        if (!tcb) continue;

        if (tcb->id.src_ip   == dst_ip   &&
            tcb->id.src_port == dst_port &&
            tcb->id.dst_ip   == src_ip   &&
            tcb->id.dst_port == src_port) {
            return i;
        }
    }

    err_sys("Can not find fd with this tuple");
    return -1;
}

ssize_t utcp_recv(int fd, void *buf, size_t len, int flags) {
    /**
     * @brief Receive from a specific fd.
    */

    struct tcb_info *tcb = utcp_get_tcb(fd);

    uint8_t packet[1500];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    ssize_t n = recvfrom(
        udp_fd,
        packet,
        sizeof(packet),
        0,
        (struct sockaddr *)&from,
        &fromlen
    );

    if (n < 0) err_sys("UTCP recvfrom failed");

    if ((size_t)n < sizeof(tcphdr)) err_sys("Header incomplete");

    struct tcp_segment *seg = (struct tcp_segment *)packet;
    tcphdr *hdr = &seg->hdr;

    uint32_t src_ip   = from.sin_addr.s_addr;
    uint16_t src_port = hdr->th_sport;
    uint32_t dst_ip   = tcb->id.src_ip;
    uint16_t dst_port = tcb->id.src_port;

    int target_fd = utcp_find_tcb(src_ip, src_port, dst_ip, dst_port);
    if (target_fd < 0)
        return 0; // no socket owns this segment

    struct tcb_info *rtcb = utcp_fd_table[target_fd];

    uint32_t seq = ntohl(hdr->th_seq);
    size_t hdr_len = (hdr->th_off_flags >> 4) * 4;
    size_t payload_len = n - hdr_len;

    /* --- SYN handling --- */
    if (hdr->th_flags & TH_SYN) {
        rtcb->rcv_nxt = seq + 1;
        return 0;
    }

    /* --- data handling --- */
    if (payload_len > 0) {
        if (seq != rtcb->rcv_nxt) {
            // out-of-order, drop for now
            return 0;
        }

        size_t copy_len = payload_len < len ? payload_len : len;
        memcpy(buf, packet + hdr_len, copy_len);

        rtcb->rcv_nxt += copy_len;
        return copy_len;
    }

    return 0;
}
