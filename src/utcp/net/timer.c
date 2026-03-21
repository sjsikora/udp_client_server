#include "logging.h"
#include "utcp/net/tcp.h"
#include "utcp/net/timers.h"
#include "utcp/utcp_init.h"
#include "utcp/utcp_output.h"
#include "utcp/utcp_utils.h"
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

        // Don't go over 64 seconds
        if (new_timer > TCPTV_REXMTMAX)
            new_timer = TCPTV_REXMTMAX;
        tcb->t_timer[TCPT_REXMT] = new_timer;

        struct cc_event_args args;
        args.type = TCP_CC_EVENT_TIMEOUT;
        args.data.timeout.flight_size = tcb->snd_nxt - tcb->snd_una;

        // Rollback the sequence pointers
        uint32_t pre_rollback_snd_nxt = tcb->snd_nxt;
        tcb->snd_nxt = tcb->snd_una;

        dzlog_warn("REXMT: Timeout #%d fired! "
                   "base_rto=%d ticks (%d ms) x backoff=%d -> new_timer=%d ticks (%d ms). "
                   "flight_size=%u bytes. snd_nxt rolled back: %u -> %u (snd_una).",
                   tcb->t_rxtshift, base_rto, base_rto * TCP_TICK_MS, backoff_multiplier, new_timer,
                   new_timer * TCP_TICK_MS, args.data.timeout.flight_size, pre_rollback_snd_nxt, tcb->snd_nxt);

        /**
         * Karn's Algorithm: reset the RTT measurement. If not reset, when the
         * retransmitted packet is ACKed we cannot tell whether the ACK is for
         * the original or the retransmitted copy, so the sample would be
         * ambiguous and must not be used to update SRTT/RTTVAR.
         */
        tcb->t_rtt = 0;

        // Pass RTO expire to the respective cong_control
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
    case TCPT_DELACK:
        dzlog_debug("Delayed ACK timer expired. Forcing ACK.");
        tcb->t_flags &= ~TF_DELACK; // Clear the delayed flag
        tcb->t_flags |= TF_ACKNOW;  // Set the force-send flag
        utcp_output(tcb);           // Push the ACK
        break;
    default:
        err_sys("Unknown timer index expired");
        break;
    }
}

void *utcp_slowtimo_thread(void *arg) {
    (void)arg; // pthread requires arg, but we don't use it. Line here to prevent warning
    zlog_put_mdc("thread_name", "Slow_ticker");

    dzlog_info("Ticker thread wake up. Tick interval: %d ms", TCP_TICK_MS);

    uint64_t next_tick_time = get_current_time_ms() + TCP_TICK_MS;

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
            for (int timer = 0; timer < 5; timer++) {
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
            dzlog_warn("utcp_slowtimo missed a tick! Took longer than %d ms to process.", TCP_TICK_MS);
        }

        next_tick_time += TCP_TICK_MS;
    }

    return NULL;
}

void utcp_xmit_timer(struct tcb *tcb, uint32_t rtt_us) {
    /* Convert microsecond sample to ticks for the EWMA algorithm */
    int rtt_ticks = (int)(rtt_us / (TCP_TICK_MS * 1000U));
    if (rtt_ticks < 1)
        rtt_ticks = 1; /* minimum 1 tick; prevents EWMA collapsing to zero */

    uint32_t old_srtt_ticks = tcb->t_srtt >> 3;
    uint32_t old_rttvar_ticks = tcb->t_rttvar >> 2;
    uint32_t old_rxtcur = tcb->t_rxtcur;

    if (tcb->t_srtt == 0) {
        /**
         * First measurement ever.
         * RFC 6298 §2.2: SRTT <- R, RTTVAR <- R/2, RTO <- SRTT + 4*RTTVAR = 3*R.
         *
         * Fixed-point representation:
         *   t_srtt   is SRTT * 8   → rtt_ticks << 3
         *   t_rttvar is RTTVAR * 4 → (rtt_ticks/2) * 4 = rtt_ticks * 2 = rtt_ticks << 1
         */
        tcb->t_srtt = rtt_ticks << 3;
        tcb->t_rttvar = rtt_ticks << 1;
        dzlog_info("RTT [xmit_timer]: First measurement: R=%d ticks (%d ms). "
                   "Initialising SRTT=%d ticks (%d ms), RTTVAR=%d ticks (%d ms).",
                   rtt_ticks, rtt_ticks * TCP_TICK_MS, tcb->t_srtt >> 3, (tcb->t_srtt >> 3) * TCP_TICK_MS,
                   tcb->t_rttvar >> 2, (tcb->t_rttvar >> 2) * TCP_TICK_MS);
    } else {
        /**
         * Subsequent measurements — Jacobson/Karels algorithm (RFC 6298 §2.3).
         *
         *   delta  = R' - SRTT_real         (error between new sample and estimate)
         *   SRTT   = SRTT + (1/8) * delta   (exponential weighted moving average, α=1/8)
         *   RTTVAR = RTTVAR + (1/4)*(|delta| - RTTVAR)  (variance estimate, β=1/4)
         *
         * In fixed-point (t_srtt scaled ×8, t_rttvar scaled ×4):
         *   t_srtt  += delta              (delta cancels the ×8 scale)
         *   t_rttvar += |delta| - (t_rttvar >> 2)
         */
        int delta = rtt_ticks - (int)(tcb->t_srtt >> 3);

        tcb->t_srtt += delta; /* SRTT ← SRTT + (1/8)·delta  (scaled ×8) */

        if (delta < 0)
            delta = -delta;                 /* |delta| */
        delta -= (int)(tcb->t_rttvar >> 2); /* |delta| − RTTVAR_real */
        tcb->t_rttvar += delta;             /* RTTVAR ← RTTVAR + (1/4)·(|delta|−RTTVAR) */

        dzlog_debug("RTT [xmit_timer]: R=%d ticks (%d ms), delta=%+d ticks.", rtt_ticks, rtt_ticks * TCP_TICK_MS,
                    rtt_ticks - (int)old_srtt_ticks);
    }

    // RTO = SRTT + 4 * RTTVAR  (using scaled fields: (t_srtt>>3) + t_rttvar)
    tcb->t_rxtcur = (tcb->t_srtt >> 3) + tcb->t_rttvar;

    // Enforce RFC 6298 minimum (200 ms) and implementation maximum (64 s).
    if (tcb->t_rxtcur < TCPTV_MIN) {
        dzlog_debug("RTT [xmit_timer]: RTO %u ticks clamped up to TCPTV_MIN=%d ticks.", tcb->t_rxtcur, TCPTV_MIN);
        tcb->t_rxtcur = TCPTV_MIN;
    } else if (tcb->t_rxtcur > TCPTV_REXMTMAX) {
        dzlog_debug("RTT [xmit_timer]: RTO %u ticks clamped down to TCPTV_REXMTMAX=%d ticks.", tcb->t_rxtcur,
                    TCPTV_REXMTMAX);
        tcb->t_rxtcur = TCPTV_REXMTMAX;
    }

    dzlog_info("RTT [xmit_timer]: measured=%d ticks (%d ms) | "
               "srtt: %u→%u ticks (%u→%u ms) | "
               "rttvar: %u→%u ticks (%u→%u ms) | "
               "rxtcur: %u→%u ticks (%u→%u ms)",
               rtt_ticks, rtt_ticks * TCP_TICK_MS, old_srtt_ticks, tcb->t_srtt >> 3, old_srtt_ticks * TCP_TICK_MS,
               (tcb->t_srtt >> 3) * TCP_TICK_MS, old_rttvar_ticks, tcb->t_rttvar >> 2, old_rttvar_ticks * TCP_TICK_MS,
               (tcb->t_rttvar >> 2) * TCP_TICK_MS, old_rxtcur, tcb->t_rxtcur, old_rxtcur * TCP_TICK_MS,
               tcb->t_rxtcur * TCP_TICK_MS);

    /* RTT time-series CSV row: seq, rtt_us, srtt_us, rttvar_us, rto_ms */
    uint32_t srtt_us_log   = (tcb->t_srtt   >> 3) * TCP_TICK_MS * 1000;
    uint32_t rttvar_us_log = (tcb->t_rttvar >> 2) * TCP_TICK_MS * 1000;
    uint32_t rto_ms_log    = tcb->t_rxtcur  * TCP_TICK_MS;
    zlog_info(rtt_logger, "%u,%u,%u,%u,%u",
              tcb->t_rtseq, rtt_us, srtt_us_log, rttvar_us_log, rto_ms_log);
}
