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
 * with utcp_bind() with no issue (as longe as you also then talk to a UTCP server).
 */
#include <netinet/in.h>
#include <utcp/net/tcp.h>
#ifndef MOCK_TCP_H
#define MOCK_TCP_H

#define MAX_UTCP_SOCKETS 6

extern struct tcb *utcp_fd_table[MAX_UTCP_SOCKETS];

/*
 * @brief Creates a UTCP socket.
 *
 * This function will create a utcp socket by initilizating a tcb
 * for the session.
 *
 * @return int A utcp file descriptor that references the socket.
 * @note Exits the program if underlying UDP socket creation fails
 */
int utcp_socket(void);

/*
 * @brief Binds a sockaddr to a UTCP port
 *
 * Populates the LOCAL side of side of a UTCP socket. Notice, there are
 * some special things happening here. The port that is given by the
 * sockaddr is not a port that you would see anywhere else. Rather, it is a
 * UTCP, our own special port that we track. To mimic the BS API, given addr
 * as if it was a true bind function (given information in network order).
 *
 */
int utcp_bind(int fd, const struct sockaddr *, socklen_t addrlen);

/*
 * @brief Connect to another UTCP socket in addr
 *
 * Initlizes the three-way handshake.
 */
int utcp_connect(int fd, const struct sockaddr *, socklen_t addrlen);

/*
 * @brief Listens for SYNs on this fd and will execute the
 * three way handshake.
 *
 * It does this through the following steps:
 *
 * 1. Listen for SYN connections on the global UDP port
 * 2. Once a packet comes, parse to TCP header
 * 3. If wrong port, error out
 * 4. If correct port, send SYN-ACK back
 */
int utcp_listen(int fd);

/**
 *
 */
int utcp_accept(int fd);

int utcp_send(int fd, const void *buf, size_t len);

int utcp_read(int fd, uint8_t *buf, size_t len);

/*
 * @brief Initializes the utcp (TCP-over-UDP) package
 *
 * This function will allocate a UDP socket, and bind information
 * to that port. This will allow traffic to follow through the
 * UDP and into our UTCP sockets.
 *
 */
void utcp_package_init(int local_udp_port);

#endif
