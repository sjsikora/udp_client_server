#include "utcp/api.h"
#include "utcp/config.h"
#include "utcp/net/tcp.h"
#include "utcp/net/timers.h"
#include "utcp/utcp_input.h"
#include "utcp/utcp_utils.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <utils.h>
#include <zlog.h>

static void start_listening();
static void start_ticking();

int             UDP_PORT = -1; // Host order global UDP port
static int      utcp_initialized = 0;
int             udp_fd = -1;
struct tcb     *utcp_fd_table[MAX_UTCP_SOCKETS] = {0};
pthread_mutex_t utcp_table_lock;
unsigned int    random_seed = 67;

void utcp_package_init(int global_udp_port) {
    if (utcp_initialized) {
        dzlog_debug("UTCP package already initialized. Skipping.");
        return;
    }

    // Bind a UDP port on IPv4 on given port
    dzlog_info("Initializing UTCP package on requested UDP port %d...", global_udp_port);

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,                // Listen on IPv4
        .sin_port = htons(global_udp_port),   // Listen on port UDP_PORT
        .sin_addr.s_addr = htonl(INADDR_ANY), // Allow external network traffic
    };

    if ((udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
        err_sys("socketerror");
    if (bind(udp_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
        err_sys("Bind failed for local UDP port");

    // The passed port may have been ephemeral, get the true port number

    struct sockaddr_in bound_addr;
    socklen_t          addrlen = sizeof(bound_addr);

    if (getsockname(udp_fd, (struct sockaddr *)&bound_addr, &addrlen) < 0)
        err_sys("getsockname failed");

    UDP_PORT = ntohs(bound_addr.sin_port);
    dzlog_info("UTCP package is initialized and successfully bound to true UDP port %u", UDP_PORT);

    // Init global utcp_fd lock
    pthread_mutex_init(&utcp_table_lock, NULL);

    utcp_initialized = 1;

    start_listening();
    start_ticking();
}

/**
 * Starts a thread that runs utcp_input
 */
static void start_listening() {
    pthread_t input_thread;

    if (pthread_create(&input_thread, NULL, utcp_input, NULL) != 0) {
        err_sys("Failed to create utcp_input thread");
    }
    // Detach so we don't have to join it later
    pthread_detach(input_thread);
}

static void start_ticking() {
    pthread_t ticking_thread;

    if (pthread_create(&ticking_thread, NULL, (void *(*)(void *))utcp_slowtimo_thread, NULL) != 0) {
        err_sys("Failed to create utcp_slowtimo thread");
    }

    pthread_detach(ticking_thread);
}
