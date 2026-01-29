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
 *
*/



#include <udp_client_server/net/utcp_api.h>
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

void err_sys(const char* x)
{
    perror(x);
    exit(1);
}

void dump_tcb(int fd)
{
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
     * @brief Initializes the utcp (TCP-over-UDP) stack
     *
     * This function will allocate a UDP socket, (erroring out if this fails).
     * Then, it will set the global utcp state to initialized.
    */

    if (utcp_initialized) return;

    if ( (udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) err_sys("socketerror");

    utcp_initialized = 1;
}

static bool utcp_fd_is_valid(int fd)
{
    /**
     * @brief Checks to see if the utcp fd has been allocated.
     *
     * Currently, ensures that the fd is within the legal bounds
     * and that there exists a tcb at that fd in the global fd
     * table.
     *
     * @return Bool indicating utcp is valid (true) or not (false)
    */

    if (fd < 0 || fd >= MAX_UTCP_SOCKETS)
    { // Incorrect file descriptor
        err_sys("Incorrect utcp file descriptor");
        return false;
    }

    struct tcb_info *tcb_at_fd = utcp_fd_table[fd];

    if (!tcb_at_fd)
    { // TCB never initilized
        err_sys("No socket at this fd");
        return false;
    }

    if (tcb_at_fd -> state == 0)
    {
        err_sys("Socket in invalid state");
        return false;
    }

    return true;
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

    // Sequence numbers initialized later (connect/listen)
    tcb->snd_una = 0;
    tcb->snd_nxt = 0;
    tcb->rcv_nxt = 0;

    utcp_fd_table[utcp_fd] = tcb;
    return utcp_fd;

}

static int utcp_port_is_in_use(uint16_t port)
{
    for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
        struct tcb_info *t = utcp_fd_table[i];
        if (!t)
            continue;

        if (t->id.src_port == htons(port))
            return 1;
    }
    return 0;
}


int utcp_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    /**
     * @brief Binds a sockaddr to a UTCP port
     *
     * Populates the LOCAL side of side of a UTCP socket. Notice, there are
     * some special things happening here. The port that is given by the sockaddr
     * is not a port that you would see anywhere else. Rather, it is a UTCP, our own
     * special port that we track.
     *
     */

    if(!utcp_fd_is_valid(fd)) err_sys("Invalid utcp file descriptor!");

    struct tcb_info *tcb = utcp_fd_table[fd];
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
