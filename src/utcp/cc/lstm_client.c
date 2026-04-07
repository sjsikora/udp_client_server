#include "utcp/cc/lstm_client.h"

#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LSTM_SOCK_PATH "/tmp/utcp_lstm.sock"

static int g_sock  = -1;
static int g_fired = 0;  /* 1 = Python server signalled congestion imminent */

int lstm_client_init(void) {
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, LSTM_SOCK_PATH, sizeof(addr.sun_path) - 1);

    /* Connect with a blocking socket — Unix domain socket connect is
     * synchronous, and a non-blocking connect fails immediately on macOS
     * instead of returning EINPROGRESS like a TCP socket would. */
    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    /* Switch to non-blocking only after the connection is established so
     * that all subsequent send/recv calls never stall the CC path. */
    int flags = fcntl(g_sock, F_GETFL, 0);
    if (flags < 0 || fcntl(g_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    return 0;
}

void lstm_client_send_row(uint64_t ts_us,
                          uint32_t rtt_us,    uint32_t srtt_us,
                          uint32_t rttvar_us, uint32_t rto_us,
                          uint64_t min_rtt_us,
                          int64_t  queue_delay_us,
                          int32_t  rtt_delta_us,
                          int32_t  rtt_accel_us,
                          int32_t  rto_delta_us,
                          uint32_t cwnd,       uint32_t ssthresh,
                          uint32_t snd_wnd,    uint32_t flight_size,
                          uint32_t newly_acked,
                          uint64_t inter_ack_us,
                          uint32_t ca_state,   uint32_t t_dupacks,
                          uint32_t t_rxtshift,
                          uint32_t is_dup_ack, uint32_t is_timeout) {
    if (g_sock < 0)
        return;

    /* Drain all pending fired bytes; any 1 in the batch means the model fired. */
    uint8_t b;
    while (recv(g_sock, &b, sizeof(b), MSG_DONTWAIT) == (ssize_t)sizeof(b))
        g_fired = b ? 1 : 0;

    /* Format the row exactly as logger.c does (minus the zlog_ts prefix). */
    char buf[512];
    int  len = snprintf(buf, sizeof(buf),
                        "%llu,%u,%u,%u,%u,%llu,"
                        "%lld,%d,%d,%d,"
                        "%u,%u,%u,%u,%u,"
                        "%llu,%u,%u,%u,%u,%u\n",
                        (unsigned long long)ts_us,
                        rtt_us, srtt_us, rttvar_us, rto_us,
                        (unsigned long long)min_rtt_us,
                        (long long)queue_delay_us, rtt_delta_us,
                        rtt_accel_us, rto_delta_us,
                        cwnd, ssthresh, snd_wnd, flight_size, newly_acked,
                        (unsigned long long)inter_ack_us,
                        ca_state, t_dupacks, t_rxtshift, is_dup_ack, is_timeout);

    /* Non-blocking send — silently drop if the kernel buffer is full. */
    (void)send(g_sock, buf, (size_t)len, MSG_DONTWAIT);
}

int lstm_client_fired(void) {
    return g_fired;
}

void lstm_client_close(void) {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}
