#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utcp/api.h>

int main(int argc, char **argv) {

    int fd = utcp_socket();

    /* Bind UTCP 'local port' 7654 */
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(7654), // UTCP port
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));

    /* Blocking listen for a SYN and perform handshake */
    utcp_listen(fd);
    utcp_accept(fd);

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

    char *msg = "I am coming to your cottage.";

    utcp_send(fd, msg, strlen(msg));

    return 0;
}
