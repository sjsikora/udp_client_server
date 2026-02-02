#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <udp_client_server/net/utcp_api.h>

int main(int argc, char **argv) {

    int fd = utcp_socket();

    /* Bind UTCP “local port” 7654 */
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(7654), // UTCP port
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));

    /* Blocking listen for a SYN and perform handshake */
    utcp_listen_for_syn(fd);

    return 0;
}
