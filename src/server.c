#include "logging.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utcp/api.h>
#include <zlog.h>
#define TARGET_SIZE (10 * 1024 * 1024)

int main() {
    init_zlog(0);
    int                fd = utcp_socket();
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(7654), .sin_addr.s_addr = inet_addr("127.0.0.1")};

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    utcp_listen(fd);
    utcp_accept(fd);

    FILE    *out_file = fopen("received_file.txt", "wb");
    uint8_t *recv_buff = malloc(65536);
    size_t   total_received = 0;

    printf("Server: Ready. Receiving 10MB...\n");

    while (total_received < TARGET_SIZE) {
        ssize_t n = utcp_read(fd, recv_buff, 65536);
        if (n > 0) {
            fwrite(recv_buff, 1, n, out_file);
            total_received += n;
        } else if (n == 0)
            break;
    }

    printf("\nServer: Done! Received %zu bytes total.\n", total_received);
    fclose(out_file);
    free(recv_buff);
    return 0;
}
