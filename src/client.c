#include "utcp/api.h"
#include "utcp/utcp_init.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utcp/config.h>

int main(int argc, char **argv) {
    utcp_package_init(0);
    int fd = utcp_socket();

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(8292),
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    struct sockaddr_in so = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1"), .sin_port = htons(7654)};

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    utcp_connect(fd, (struct sockaddr *)&so, sizeof(so));

    char *msg = "Will you... come to my cottage this summer?";

    utcp_send(fd, msg, strlen(msg) + 1);

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
}
