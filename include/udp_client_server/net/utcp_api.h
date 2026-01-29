
#include <netinet/in.h>
#ifndef MOCK_TCP_H
#define MOCK_TCP_H


typedef struct
{
    int udp_sockfd;             /* datagram, udp socket file descriptor */
    struct tcb_info *sessions;    // dynamic array or hash table of active TCBs
    short int n_sessions;
} mck_tcp_port;

int utcp_socket(void);
int utcp_bind(int fd, const struct sockaddr *, socklen_t addrlen);
int utcp_connect(int fd, const struct sockaddr *, socklen_t addrlen);

void dump_tcb(int fd);

struct mck_tcp_port *init_socket(int);

#endif
