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
#include <utcp/utcp_init.h>
#include <unistd.h>
#include <utcp/utcp_output.h>

/**
 * Spin wait until the tcb is established. We are able to do this because
 * (in theory) utcp_input should be recieving packets and sending out the
 * corresponding packets for three-way handshake.
 */
static void wait_until_established(struct tcb *tcb) {
    while (tcb->state != TCP_ESTABLISHED) {
        usleep(1000);
    }
}

static struct tcb *utcp_get_tcb(int fd) {
    /**
     * @brief Retieve a utcp tcb by fd
     *
     * Validates the file descriptor and tcb. Errors if
     * fd is invalid (such as -1) or tcb was never allocated
     * for the fd
     */

    if (fd < 0 || fd >= MAX_UTCP_SOCKETS)
        err_sys("Invalid UTCP fd");

    struct tcb *tcb = utcp_fd_table[fd];
    if (!tcb)
        err_sys("UTCP fd not allocated");

    return tcb;
}

static struct tcb *utcp_get_tcb_in_state(int fd, enum tcp_state required) {
    /**
     * @brief Retieve a utcp tcb by fd and verify it is in a state
     */
    struct tcb *tcb = utcp_get_tcb(fd);

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
    struct tcb *tcb = calloc(1, sizeof(struct tcb));
    if (!tcb)
        err_sys("calloc failed for TCB");

    tcb->state = TCP_CLOSED;

    // Init acknowledgment numbers
    tcb->iss = 0;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;

    utcp_fd_table[utcp_fd] = tcb;
    return utcp_fd;
}

int utcp_send(int fd, const void *buf, size_t len) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_ESTABLISHED);

    // Check if there is room in buffer. Very very limited right now.
    uint32_t current_buffered = tcb->send_buf_tail - tcb->send_buf_head;
    if (current_buffered + len > SEND_BUF_SIZE) err_sys("Full buffer can not add data");

    // Add data to the send buffer
    for (size_t i = 0; i < len; i++) {
        tcb->send_buf[(tcb->send_buf_tail + i) % SEND_BUF_SIZE] = ((uint8_t *)buf)[i];
    }

    tcb->send_buf_tail += len;

    // Try to send the data
    return utcp_output(tcb);
}

int utcp_read(int fd, uint8_t *buf, size_t len) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_ESTABLISHED);

    // Spin wait for data if there is nothing to read in the buffer
    while (tcb->recv_buf_head == tcb->recv_buf_tail) {
        if (tcb->state == TCP_CLOSE_WAIT || tcb->state == TCP_CLOSED) {
            return 0;
        }
        usleep(1000000);
    }

    // Look inside the read buffer, read up to passed in buffer length,
    // or towards the data
    uint32_t avaiable_bytes_to_read = tcb->send_buf_head - tcb->send_buf_tail;
    size_t num_bytes_to_read = (len < (size_t)avaiable_bytes_to_read) ? len : (size_t)avaiable_bytes_to_read;

    for (size_t i = 0; i < num_bytes_to_read; i++) {
        buf[i] = tcb->recv_buf[(tcb->recv_buf_head + i) % RECV_BUF_SIZE];
    }

    tcb->recv_buf_head += num_bytes_to_read;

    // TODO: Increase rcv_wnd variable
}

int utcp_accept(int fd) {
    struct tcb *tcb = utcp_get_tcb(fd);

    /**
     * We may be already established because of the utcp_input thread
     * that is always listening.
     */
    if (tcb->state != TCP_LISTEN && tcb->state != TCP_SYN_RECV) {
        if (tcb->state == TCP_ESTABLISHED) return fd;
        err_sys("utcp_accept called on a socket that isn't listening");
    }

    printf("[UTCP] utcp_accept: Waiting for incoming handshake...\n");

    /**
     * Spin waits until there is a tcb connection. Note, in the future and
     * with real TCP connections, we would make a new TCB and leave the orginal
     * socket alone. However, for now, we assume one client.
     */
    wait_until_established(tcb);

    printf("[UTCP] utcp_accept: Connection established!\n");

    return 0;
}

int utcp_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSED);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    if (tcb->state != TCP_CLOSED)
        err_sys("TCB is in a invalid state for binding");
    if (tcb->src_ip != 0)
        err_sys("UTCP socket is already bound");
    if (sin->sin_family != AF_INET)
        err_sys("No support for UTCP ports that are not AF_INET");

    // Assume we were given the port in network order TCB holds in host order
    tcb->src_ip = ntohl(sin->sin_addr.s_addr);
    tcb->src_port = ntohs(sin->sin_port);

    return 0;
}

int utcp_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSED);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    if (sin->sin_family != AF_INET)
        err_sys("Only AF_INET supported for UTCP connect");

    tcb->dst_ip = ntohl(sin->sin_addr.s_addr);
    tcb->dst_port = ntohs(sin->sin_port);
    tcb->dst_udp_port = 1970; // UTCP server

    tcb->state = TCP_SYN_SENT;
    utcp_output(tcb);

    wait_until_established(tcb);
}

int utcp_listen(int fd) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSED);
    tcb->state = TCP_LISTEN;
}

static int utcp_find_tcb(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                         uint16_t dst_port) {

    for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
        struct tcb *tcb = utcp_fd_table[i];
        if (!tcb)
            continue;

        if (tcb->src_ip == dst_ip && tcb->src_port == dst_port &&
            tcb->dst_ip == src_ip && tcb->dst_port == src_port) {
            return i;
        }
    }

    err_sys("Can not find fd with this tuple");
    return -1;
}
