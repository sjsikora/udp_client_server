#include "utcp/net/tcp.h"
#include "utcp/net/timers.h"
#include "utcp/utcp_init.h"
#include "utcp/utcp_output.h"
#include "utils.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint64_t get_current_time_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        err_sys("Can not get current time");
    }

    // Convert seconds to ms, and nanoseconds to ms, then combine
    uint64_t time_ms = (uint64_t)(ts.tv_sec * 1000) + (uint64_t)(ts.tv_nsec / 1000000);

    return time_ms;
}

void utcp_timers(struct tcb *tcb, int timer) {
    switch (timer) {
    /**
     * The big bad restransmission timer. This means that there was a timeout.
     * We sent data to the sender, and we never heard back an acknowledgement of
     * our data.
     *
     * This is critcal in our cogestion avoidance algorthiums to handle and update.
     */
    case TCPT_REXMT:
        tcb->t_rxtshift++;

        // Every time we trigger a retransmission for a packet, we need a back_off_multiplier
        int backoff_multiplier = tcp_backoff[tcb->t_rxtshift];

        // TODO: RTO calculations
        int new_timer = 2 * backoff_multiplier;

        // Don't go over 128 or 64 seconds
        if (new_timer > 128)
            new_timer = 128;
        tcb->t_timer[TCPT_REXMT] = new_timer;

        // Rollback the sequence pointers
        tcb->snd_nxt = tcb->snd_una;
        printf("[UTCP] RTO Expired! Retransmitting sequence %u\n", tcb->snd_nxt);

        // Force retransmission
        utcp_output(tcb);

    /**
     * Fired when the receiver advertised a window of 0, and we
     * are waiting to see if it opened up.
     */
    case TCPT_PERSIST:
        break;
    /**
     * Fired when a SYN isn't answered, or a connection goes idle.
     */
    case TCPT_KEEP:
        break;
    /**
     * Fired during connection teardown to ensure old packets die.
     */
    case TCPT_2MSL:
        break;
    default:
        err_sys("Unknown timer index expired");
        break;
    }
}

void *utcp_slowtimo_thread(void *arg) {

    uint64_t next_tick_time = get_current_time_ms() + TCP_SLOW_TICK_MS;

    while (1) {
        for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
            struct tcb *tcb = utcp_fd_table[i];
            if (!tcb || tcb->state == TCP_CLOSED)
                continue;

            // Increase the ticks since last segment was recevied.
            tcb->t_idle++;

            // Increase the RTT for a specific packet if we are measuring
            if (tcb->t_rtt > 0) {
                tcb->t_rtt++;
            }

            // Decrement active timers
            for (int timer = 0; timer < 4; timer++) {
                if (tcb->t_timer[timer] > 0) {
                    tcb->t_timer[timer]--;

                    // Did the timer just expire?
                    if (tcb->t_timer[timer] == 0) {
                        utcp_timers(tcb, timer);
                    }
                }
            }
        }

        /**
         * Now, we are done with our work, but it has taken us n ms to do this
         * if we slept for 500ms, then we would have timer drift. So, we want to
         * calc what time we need to sleep for to match the next expected tick
         */
        uint64_t now = get_current_time_ms();

        // Sleep only for the remaining time until the next absolute tick
        if (now < next_tick_time) {
            uint64_t sleep_time_ms = next_tick_time - now;
            usleep(sleep_time_ms * 1000);
        } else {
            // Warning: We took longer than 500ms to process!
            printf("[WARNING] utcp_slowtimo missed a tick!\n");
        }

        next_tick_time += TCP_SLOW_TICK_MS;
    }

    return NULL;
}
