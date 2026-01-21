#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(7654)
    };
    char buffer[1024];
    ssize_t recsize;
    socklen_t fromlen = sizeof(sa);

    int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bind(sock, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        fprintf(stderr, "Failed to bind socket!\n");
        close(sock);
        return EXIT_FAILURE;
    }

    while (true) {
        recsize = recvfrom(sock, (void*)buffer, sizeof buffer, 0, (struct sockaddr*)&sa, &fromlen);
        if (recsize < 0) {
            fprintf(stderr, "%s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        printf("recsize: %d\n ", (int)recsize);
        sleep(1);
        printf("datagram: %.*s\n", (int)recsize, buffer);
    }
}
