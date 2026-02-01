#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <udp_client_server/net/utcp_api.h>

int main(int argc, char **argv)
{
    printf("[UTCP server] starting...\n");

    int fd = utcp_socket();
    printf("[UTCP server] socket fd = %d\n", fd);

    /* Bind UTCP “local port” 7654 */
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(7654),          // UTCP port
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    printf("[UTCP server] bound to UTCP port 7654\n");

    dump_tcb(fd);

    printf("[UTCP server] waiting for SYN...\n");

    /* Blocking listen for a SYN and perform handshake */
    utcp_listen_for_syn(fd);

    printf("[UTCP server] handshake complete\n");
    dump_tcb(fd);

    /* For now just sleep so process doesn’t exit */
    while (1) {
        sleep(1);
    }

    return 0;
}
