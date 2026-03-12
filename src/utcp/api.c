#include "utcp/cc/reno.h"
#include "utcp/cc/tahoe.h"
#include "utcp/net/timers.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utcp/api.h>
#include <utcp/net/tcp.h>
#include <utcp/utcp_init.h>
#include <utcp/utcp_output.h>
#include <utcp/utcp_utils.h>
#include <utils.h>
#include <zlog.h>

/**
 * Spin wait until the tcb is established. We are able to do this because
 * (in theory) utcp_input should be recieving packets and sending out the
 * corresponding packets for three-way handshake.
 */
static void wait_until_established(struct tcb *tcb) {
    while (tcb->state != TCP_ESTABLISHED) {
        usleep(1000);
    }
    dzlog_debug("TCB state is now TCP_ESTABLISHED.");
}

/**
 * @brief Retieve a utcp tcb by fd
 *
 * Validates the file descriptor and tcb. Errors if
 * fd is invalid (such as -1) or tcb was never allocated
 * for the fd
 */
static struct tcb *utcp_get_tcb(int fd) {

    if (fd < 0 || fd >= MAX_UTCP_SOCKETS)
        err_sys("Invalid UTCP fd");

    struct tcb *tcb = utcp_fd_table[fd];
    if (!tcb)
        err_sys("UTCP fd not allocated");

    return tcb;
}

/**
 * @brief Retieve a utcp tcb by fd and verify it is in a state
 */
static struct tcb *utcp_get_tcb_in_state(int fd, enum tcp_state required) {

    struct tcb *tcb = utcp_get_tcb(fd);

    if (tcb->state != required)
        err_sys("UTCP socket in invalid state");

    return tcb;
}

int utcp_socket(void) {
    utcp_package_init(1970);

    pthread_mutex_lock(&utcp_table_lock);

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

    // Init locks
    pthread_mutex_init(&tcb->lock, NULL);
    pthread_cond_init(&tcb->cond_var, NULL);

    // tcb->cc_ops = &utcp_tahoe;
    tcb->cc_ops = &utcp_reno;

    // Set timer to default value
    tcb->t_rxtcur = TCPTV_SRTTDFLT;

    // Init acknowledgment numbers
    tcb->iss = 0;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->rcv_wnd = RECV_BUF_SIZE;

    // Dynamic window scale calculation
    uint8_t scale = 0;
    while (RECV_BUF_SIZE >> scale > 65535 && scale < 14) {
        scale++;
    }
    tcb->rcv_scale = scale;
    tcb->scale_enabled = false;
    tcb->snd_scale = 0;

    utcp_fd_table[utcp_fd] = tcb;
    pthread_mutex_unlock(&utcp_table_lock);

    dzlog_info("Successfully allocated UTCP socket fd %d", utcp_fd);
    return utcp_fd;
}

void utcp_send(int fd, const void *buf, size_t len) {
    struct tcb    *tcb = utcp_get_tcb_in_state(fd, TCP_ESTABLISHED);
    const uint8_t *data_ptr = (const uint8_t *)buf;
    size_t         remaining = len;

    pthread_mutex_lock(&tcb->lock);

    while (remaining > 0) {
        uint32_t current_buffered = tcb->send_buf_tail - tcb->send_buf_head;
        uint32_t free_space = SEND_BUF_SIZE - current_buffered;

        if (free_space == 0) {
            // Buffer is full. Sleep until utcp_input processes an ACK and wakes us up.
            dzlog_debug("Send buffer full on fd %d, blocking application thread...", fd);
            pthread_cond_wait(&tcb->cond_var, &tcb->lock);

            // If the connection drops while we are asleep, we need to bail out
            if (tcb->state != TCP_ESTABLISHED) {
                err_sys("Connection closed while blocked in utcp_send");
                break;
            }
            continue; // Re-evaluate free space
        }

        // Write whatever chunk we have space for
        size_t to_write = (remaining < free_space) ? remaining : free_space;

        ring_buf_write(tcb->send_buf, SEND_BUF_SIZE, tcb->send_buf_tail, data_ptr, to_write);

        tcb->send_buf_tail += to_write;
        data_ptr += to_write;
        remaining -= to_write;

        dzlog_debug("Added %zu bytes to send buffer on fd %d. %zu bytes remaining.", to_write, fd, remaining);

        // Try to push this chunk out to the network
        utcp_output(tcb);
    }

    pthread_mutex_unlock(&tcb->lock);

    return;
}

int utcp_read(int fd, uint8_t *buf, size_t len) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_ESTABLISHED);
    pthread_mutex_lock(&tcb->lock);

    // Spin wait for data if there is nothing to read in the buffer
    while (tcb->recv_buf_head == tcb->recv_buf_tail) {
        if (tcb->state == TCP_CLOSE_WAIT || tcb->state == TCP_CLOSED) {
            dzlog_error("Socket %d closed or closing during read. Returning 0 bytes.", fd);
            pthread_mutex_unlock(&tcb->lock);
            return 0;
        }
        // Release the lock, sleep, and re-acquire when signaled
        pthread_cond_wait(&tcb->cond_var, &tcb->lock);
    }

    // Look inside the read buffer, read up to passed in buffer length,
    // or read all the data avaiable in the buffer
    uint32_t avaiable_bytes_to_read = tcb->recv_buf_tail - tcb->recv_buf_head;
    size_t   num_bytes_to_read = (len < (size_t)avaiable_bytes_to_read) ? len : (size_t)avaiable_bytes_to_read;

    ring_buf_read(tcb->recv_buf, RECV_BUF_SIZE, tcb->recv_buf_head, buf, num_bytes_to_read);

    tcb->recv_buf_head += num_bytes_to_read;

    // When the application has read the payload, we can free up the receieve window
    // that is advertised to the sender. Recalculate this here.
    // ooo_bytes are reserved for OOO segments that will drain into recv_buf.
    uint32_t bytes_in_buffer = tcb->recv_buf_tail - tcb->recv_buf_head;
    tcb->rcv_wnd = RECV_BUF_SIZE - bytes_in_buffer - tcb->ooo_bytes;

    // Silly window prevention with Classic Clark's algorithm: only send window update when
    // we can offer at least min(MSS, RECV_BUF_SIZE/2) worth of new space.
    uint32_t sws_threshold = (MSS < RECV_BUF_SIZE / 2) ? MSS : RECV_BUF_SIZE / 2;
    if (tcb->rcv_wnd >= sws_threshold || (tcb->rcv_wnd < sws_threshold && avaiable_bytes_to_read == RECV_BUF_SIZE)) {
        dzlog_debug("SWS triggered on fd %d: Sending window update (rcv_wnd=%u)", fd, tcb->rcv_wnd);
        tcb->t_flags |= TF_ACKNOW;
        utcp_output(tcb);
    }

    pthread_mutex_unlock(&tcb->lock);

    dzlog_debug("Successfully read %zu bytes from fd %d", num_bytes_to_read, fd);

    return (int)num_bytes_to_read;
}

int utcp_accept(int fd) {
    struct tcb *tcb = utcp_get_tcb(fd);

    /**
     * We may be already established because of the utcp_input thread
     * that is always listening.
     */
    if (tcb->state != TCP_LISTEN && tcb->state != TCP_SYN_RECV) {
        if (tcb->state == TCP_ESTABLISHED)
            dzlog_info("Socket %d is already ESTABLISHED, accepting immediately.", fd);
        return fd;
        err_sys("utcp_accept called on a socket that isn't listening");
    }

    dzlog_debug("Spin waiting for a incoming handshake.");

    /**
     * Spin waits until there is a tcb connection. Note, in the future and
     * with real TCP connections, we would make a new TCB and leave the orginal
     * socket alone. However, for now, we assume one client.
     */
    wait_until_established(tcb);

    dzlog_debug("Connection established!");

    return 0;
}

int utcp_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    struct tcb               *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSED);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    (void)addrlen; // Addrlen here to mimic Berkeley Sockets API. This line prevents warning

    if (tcb->state != TCP_CLOSED)
        err_sys("TCB is in a invalid state for binding");
    if (tcb->src_ip != 0)
        err_sys("UTCP socket is already bound");
    if (sin->sin_family != AF_INET)
        err_sys("No support for UTCP ports that are not AF_INET");

    tcb->src_ip = ntohl(sin->sin_addr.s_addr);
    tcb->src_port = ntohs(sin->sin_port);

    dzlog_info("Successfully bound fd %d to port %u", fd, tcb->src_port);

    return 0;
}

int utcp_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    struct tcb               *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSED);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    (void)addrlen; // Addrlen here to mimic Berkeley Sockets API. This line prevents warning

    pthread_mutex_lock(&tcb->lock);

    if (sin->sin_family != AF_INET)
        err_sys("Only AF_INET supported for UTCP connect");

    tcb->dst_ip = ntohl(sin->sin_addr.s_addr);
    tcb->dst_port = ntohs(sin->sin_port);
    tcb->dst_udp_port = 1970; // UTCP server hardcoded for now

    tcb->state = TCP_SYN_SENT;
    dzlog_debug("Sending SYN for fd %d...", fd);
    utcp_output(tcb);

    pthread_mutex_unlock(&tcb->lock); // Unlock here because the listen thread will handle the rest

    wait_until_established(tcb);
    dzlog_info("Successfully connected fd %d!", fd);

    return 0;
}

int utcp_listen(int fd) {
    struct tcb *tcb = utcp_get_tcb_in_state(fd, TCP_CLOSED);
    tcb->state = TCP_LISTEN;
    return 0;
}
