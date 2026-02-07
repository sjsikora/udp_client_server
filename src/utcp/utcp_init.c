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
#include <utcp/config.h>

int UDP_PORT = -1; // Host order global UDP port
static int utcp_initialized = 0;
static int udp_fd = -1;
struct tcb *utcp_fd_table[MAX_UTCP_SOCKETS] = {0};

void utcp_package_init(int global_udp_port) {
    if (utcp_initialized) return;

    // Bind a UDP port on IPv4 on given port

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,                     // Listen on IPv4
        .sin_port = htons(global_udp_port),         // Listen on port UDP_PORT
        .sin_addr.s_addr = inet_addr("127.0.0.1"), // Listen on localhost (localhost we can see loop0 in wireshark)
    };

    if ((udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
        err_sys("socketerror");
    if (bind(udp_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
        err_sys("Bind failed for local UDP port");

    // The passed port may have been ephemeral, get the true port number

    struct sockaddr_in bound_addr;
    socklen_t addrlen = sizeof(bound_addr);

    if (getsockname(udp_fd, (struct sockaddr *)&bound_addr, &addrlen) < 0)
        err_sys("getsockname failed");

    UDP_PORT = ntohs(bound_addr.sin_port);
    printf("[UTCP] UTCP package is initlized and is listening on port %u\n",
           UDP_PORT);

    utcp_initialized = 1;

}
