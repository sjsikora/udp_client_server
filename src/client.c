#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <sys/types.h>
#include <utcp/config.h>
#include "utcp/api.h"
#include <unistd.h>

int main(int argc, char **argv) {
    utcp_package_init(0);
    int fd = utcp_socket();

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(8292),
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    struct sockaddr_in so = {.sin_family = AF_INET,
                             .sin_addr.s_addr = inet_addr("127.0.0.1"),
                             .sin_port = htons(7654)};

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    utcp_connect(fd, (struct sockaddr *)&so, sizeof(so));

    char *msg = "Will you... come to my cottage this summer?";

    utcp_send(fd, msg, strlen(msg));

    // At this point, the three way handshake as been accomplished
    char buff[100];

    ssize_t n = utcp_read(fd, buff, sizeof(buff) - 1);

    if (n > 0) {
        // Ensure the string is null-terminated for safe printing
        buff[n] = '\0';
        printf("Received %zd bytes: %s\n", n, buff);
    } else if (n == 0) {
        printf("Connection closed by peer (EOF).\n");
    } else {
        printf("Error reading from UTCP socket.\n");
    }


}
