#include "utcp/net/tcp.h"
#include "utcp/net/timers.h"
#include "utcp/utcp_init.h"
#include "utcp/utcp_output.h"
#include "utils.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <zlog.h>

const int tcp_backoff[TCP_MAXRXTSHIFT + 1] = {1, 2, 4, 8, 16, 32, 64, 64, 64, 64, 64, 64, 64};

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

        // Use dynamically calculated RTO, with a fallback to default if needed.
        int base_rto = tcb->t_rxtcur > 0 ? tcb->t_rxtcur : TCPTV_SRTTDFLT;
        int new_timer = base_rto * backoff_multiplier;

        // Don't go over 128 or 64 seconds
        if (new_timer > 128)
            new_timer = 128;
        tcb->t_timer[TCPT_REXMT] = new_timer;

        struct cc_event_args args;
        args.type = TCP_CC_EVENT_TIMEOUT;
        args.data.timeout.flight_size = tcb->snd_max - tcb->snd_una;

        // Rollback the sequence pointers
        tcb->snd_nxt = tcb->snd_una;

        dzlog_warn("RTO Expired! Retransmitting sequence %u, flight_size=%u, backoff_shift=%d", tcb->snd_nxt,
                   args.data.timeout.flight_size, tcb->t_rxtshift);

        /**
         * Reset the RTT timer. If not, when a packet was dropped and an ACK eventually arrives, our RTT timer will
         * not realize the ACK is for the second attempt and not the first. AKA Karn's Algorithm.
         */
        tcb->t_rtt = 0;

        // Pass dup ACK to the respective cong_control
        tcb->cc_ops->cong_control(tcb, &args);

        // Force retransmission
        utcp_output(tcb);

        break;

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
    zlog_put_mdc("thread_name", "Slow_ticker");

    dzlog_info("Ticker thread wake up. Tick interval: %d ms", TCP_SLOW_TICK_MS);

    uint64_t next_tick_time = get_current_time_ms() + TCP_SLOW_TICK_MS;

    while (1) {
        pthread_mutex_lock(&utcp_table_lock);

        for (int i = 0; i < MAX_UTCP_SOCKETS; i++) {
            struct tcb *tcb = utcp_fd_table[i];
            if (!tcb)
                continue;

            pthread_mutex_lock(&tcb->lock);

            if (tcb->state == TCP_CLOSED) {
                pthread_mutex_unlock(&tcb->lock);
                continue;
            }

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

            pthread_mutex_unlock(&tcb->lock);
        }

        pthread_mutex_unlock(&utcp_table_lock);

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
            dzlog_warn("utcp_slowtimo missed a tick! Took longer than %d ms to process.", TCP_SLOW_TICK_MS);
        }

        next_tick_time += TCP_SLOW_TICK_MS;
    }

    return NULL;
}

void utcp_xmit_timer(struct tcb *tcb, int rtt_ticks) {
    if (tcb->t_srtt == 0) {
        /**
         * We have no previous measurement. Therefore we calculate with:
         * First measurement: RTO = RTT + 4 * (RTT / 2)
         */
        tcb->t_srtt = rtt_ticks << 3;   // Store SRTT scaled by 8
        tcb->t_rttvar = rtt_ticks << 1; // Store RTTVAR scaled by 4 (RTTVAR = RTT/2)
    } else {
        /**
         * The following measurements are caclulated with the aplha, beta
         * learned in COSC 328. In other words, it is the Jacobson/Karels Algorithium.
         */
        // delta = R' - (SRTT / 8)
        int delta = rtt_ticks - (tcb->t_srtt >> 3);

        // SRTT = SRTT + alpha * delta (alpha is 1/8)
        tcb->t_srtt += delta;

        // RTTVAR = RTTVAR + beta * (|delta| - RTTVAR) (beta is 1/4)
        if (delta < 0)
            delta = -delta;
        delta -= (tcb->t_rttvar >> 2);
        tcb->t_rttvar += delta;
    }

    // RTO = SRTT + 4 * RTTVAR
    tcb->t_rxtcur = (tcb->t_srtt >> 3) + tcb->t_rttvar;

    // Bound the RTO to minimum and maximum values defined in your constants
    if (tcb->t_rxtcur < TCPTV_MIN) {
        tcb->t_rxtcur = TCPTV_MIN;
    } else if (tcb->t_rxtcur > TCPTV_REXMTMAX) {
        tcb->t_rxtcur = TCPTV_REXMTMAX;
    }

    dzlog_debug("RTT Update: Measured=%d ticks, SRTT=%d, RTO=%d", rtt_ticks, tcb->t_srtt >> 3, tcb->t_rxtcur);
}
