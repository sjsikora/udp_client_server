#include "logging.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <utcp/api.h>
#include <zlog.h>

int main(int argc, char **argv) {
    init_zlog(0);
    dzlog_debug("Server wake up");

    int fd = utcp_socket();

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(7654),
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));

    printf("Listening for connections...\n");
    utcp_listen(fd);
    utcp_accept(fd);

    char buff[100] = {0};
    int  total_bytes = 0;

    printf("Connection established. Waiting for message...\n");

    while (total_bytes < sizeof(buff) - 1) {
        // Force the server to read slowly in 5-byte chunks
        size_t to_read = 5;
        if (sizeof(buff) - 1 - total_bytes < to_read) {
            to_read = sizeof(buff) - 1 - total_bytes;
        }

        ssize_t n = utcp_read(fd, (uint8_t *)(buff + total_bytes), to_read);

        if (n > 0) {
            total_bytes += n;
            printf("Server: Read chunk of %zd bytes. Buffer currently: '%s'\n", n, buff);

            if (buff[total_bytes - 1] == '\0') {
                break;
            }

            // Sleep for 200ms to allow the 16-byte window to fill and client to block
            usleep(200000);
        } else if (n == 0) {
            printf("Connection closed by peer (EOF).\n");
            break;
        }
    }

    printf("\nReceived full message (%d bytes): %s\n", total_bytes, buff);

    // Send the reply
    char *reply = "I am coming to your cottage.";
    printf("Server: Sending reply...\n");
    utcp_send(fd, reply, strlen(reply) + 1);

    while (1) {
        // Keep alive to allow client to finish reading before terminating
        usleep(500000);
    }

    zlog_fini();
    return 0;
}
