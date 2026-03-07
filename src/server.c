#include "logging.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utcp/api.h>
#include <zlog.h>

int main(int argc, char **argv) {
    init_zlog(0);
    dzlog_debug("Server wake up");

    int fd = utcp_socket();

    /* Bind UTCP 'local port' 7654 */
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(7654), // UTCP port
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));

    printf("Listening for connecting...");

    /* Blocking listen for a SYN and perform handshake */
    utcp_listen(fd);
    utcp_accept(fd);

    // At this point, the three way handshake as been accomplished

    char buff[100];
    int  total_bytes = 0;

    printf("Waiting for message...\n");

    // Keep reading until we hit the buffer size limit
    while (total_bytes < sizeof(buff) - 1) {

        // Read available bytes directly into the correct offset of our buffer
        ssize_t n = utcp_read(fd, (uint8_t *)(buff + total_bytes), sizeof(buff) - 1 - total_bytes);

        if (n > 0) {
            total_bytes += n;

            // Check if the very last byte is null
            if (buff[total_bytes - 1] == '\0') {
                break;
            }
        } else if (n == 0) {
            printf("Connection closed by peer (EOF).\n");
            break;
        } else {
            printf("Error reading from UTCP socket.\n");
            break;
        }
    }

    printf("Received full message (%d bytes): %s\n", total_bytes, buff);

    char *msg = "I am coming to your cottage.";

    utcp_send(fd, msg, strlen(msg) + 1);

    while (1) {
    }

    zlog_fini();
    return 0;
}
