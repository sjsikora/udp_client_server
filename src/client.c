#include "logging.h"
#include "utcp/api.h"
#include "utcp/utcp_init.h"
#include "utils.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <zlog.h>
#define CHUNK_SIZE 65536 // 64KB chunks for reading from disk

int main() {
    init_zlog(1);
    utcp_package_init(0);
    int fd = utcp_socket();

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(8292), .sin_addr.s_addr = htonl(INADDR_ANY)};

    struct sockaddr_in so = {
        .sin_family = AF_INET, .sin_port = htons(7654), .sin_addr.s_addr = inet_addr("40.82.162.155")};

    utcp_bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    utcp_connect(fd, (struct sockaddr *)&so, sizeof(so));

    FILE *fptr = fopen("test_file.txt", "rb");
    if (!fptr)
        err_sys("Could not open test_file.txt");

    uint8_t *buffer = malloc(CHUNK_SIZE);
    size_t   n, total_sent = 0;

    printf("Client: Starting 10MB transfer...\n");
    while ((n = fread(buffer, 1, CHUNK_SIZE, fptr)) > 0) {
        utcp_send(fd, buffer, n);
        total_sent += n;
        printf("Client: Progress: %zu bytes sent\r", total_sent);
        fflush(stdout);
    }

    printf("\nClient: Finished sending file (%zu bytes).\n", total_sent);

    while (1) {
    }

    fclose(fptr);
    free(buffer);
    return 0;
}
