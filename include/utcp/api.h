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

/**
 * @brief Creates a UTCP socket.
 *
 * This function will create a utcp socket by initilizating a tcb
 * for the session.
 *
 * @return int A utcp file descriptor that references the socket.
 * @note Exits the program if underlying UDP socket creation fails
 */
int utcp_socket(void);

/**
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
 * Connects to another UTCP client. It does this by initiating the three way
 * handshake. This function blocks and spin waits until established.
 */
int utcp_connect(int fd, const struct sockaddr *, socklen_t addrlen);

/**
 * @brief Opens up this socket to be listening for incoming connections
 *
 * Internally, this function only marks this socket as in the listening
 * state. If any connections come through on this socket, this socket will
 * automatically perform the three wayhandshake (TODO we should wait on SYN_RECV
 * state until the user calls accept).
 */
int utcp_listen(int fd);

/**
 * @brief Accept an incoming connection
 *
 * Spin wait until a established connection.
 */
int utcp_accept(int fd);

/**
 * @brief Send data in buffer
 *
 * Writes the data in the buffer to the send buffer. Then, calls a request to
 * send the segment out.
 */
int utcp_send(int fd, const void *buf, size_t len);

/**
 * @brief Read data from the sender
 *
 * Read from the reciever buffer into your application buffer.
 *
 * @returns Number of bytes read.
 */
int utcp_read(int fd, uint8_t *buf, size_t len);

#endif
