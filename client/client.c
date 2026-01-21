#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
    struct sockaddr_in sa = {
        // The address is IPv4
        .sin_family = AF_INET,
        // IPv4 addresses is a uint32_t, convert a string representation of the octets to the appropriate value
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
        // sockets are unsigned shorts, htons(x) ensures x is in network byte order, set the port to 7654
        .sin_port = htons(7654)
    };
    char buffer[200];

    strcpy(buffer, "Hello, world!");

    // Create an Internet, datagram socket using UDP
    int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == -1) {
        // If socket failed to initialize, exit
        fprintf(stderr, "Failed to create socket!\n");
        return EXIT_FAILURE;
    }

    // Send message using sendto()
    int bytes_sent = sendto(sock, buffer, strlen(buffer), 0, (struct sockaddr*)&sa, sizeof(sa));

    if (bytes_sent < 0) {
        fprintf(stderr, "Error sending packet: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    close(sock); // close the socket
    return 0;
}
