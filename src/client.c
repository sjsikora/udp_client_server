#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <udp_client_server/config.h>
#include <udp_client_server/net/utcp_api.h>
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
}
