#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <udp_client_server/config.h>
#include <udp_client_server/net/utcp_api.h>

int main(int argc, char **argv)
{

    int fd = utcp_socket();

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(0),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    dump_tcb(fd);
};

