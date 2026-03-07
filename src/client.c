#include "logging.h"
#include "utcp/api.h"
#include "utcp/utcp_init.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <zlog.h>

int main(int argc, char **argv) {
    init_zlog(1);
    dzlog_debug("Client wake up");

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

    printf("Client: Attempting to send %zu bytes...\n", strlen(msg) + 1);

    // This will immediately fill the 16-byte buffer and BLOCK.
    utcp_send(fd, msg, strlen(msg) + 1);

    printf("Client: Finished sending the entire message!\n");

    // Read the server's response
    char buff[100] = {0};
    int  total_bytes = 0;

    printf("Waiting for server response...\n");
    while (total_bytes < sizeof(buff) - 1) {
        ssize_t n = utcp_read(fd, (uint8_t *)(buff + total_bytes), sizeof(buff) - 1 - total_bytes);

        if (n > 0) {
            total_bytes += n;
            if (buff[total_bytes - 1] == '\0')
                break;
        } else if (n == 0) {
            printf("Connection closed by peer (EOF).\n");
            break;
        }
    }

    printf("Received full message (%d bytes): %s\n", total_bytes, buff);
    return 0;
}
