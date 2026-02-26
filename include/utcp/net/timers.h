#ifndef TIMERS_H
#define TIMERS_H

#define TCP_SLOW_TICK_MS 500 /* In ms, how long is the slow tick timer*/

/*
 * Definitions of the TCP timers constants. These timers are counted
 * down PR_SLOWHZ times a second. Timer definitions come from the inet
 * source code.
 */
#define TCPTV_MSL 60 /* max seg lifetime (hah!) */

#define TCPTV_MIN      2   /* minimum allowable value */
#define TCPTV_REXMTMAX 128 /* max allowable REXMT value */

#define TCPTV_PERSMIN 10  /* retransmit persistence */
#define TCPTV_PERSMAX 120 /* maximum persist interval */

#define TCPTV_KEEP_INIT 150   /* initial connect keepalive */
#define TCPTV_KEEP_IDLE 14400 /* dflt time before probing */
#define TCPTV_KEEPINTVL 150   /* default probe interval */

#define TCPTV_SRTTBASE 0 /* base roundtrip time; if 0, no idea yet */
#define TCPTV_SRTTDFLT 6 /* assumed RTT if no info */

#define TCP_LINGERTIME  120 /* linger at most 2 minutes */
#define TCP_MAXRXTSHIFT 12  /* maximum retransmits */
#define TCPTV_KEEPCNT   8   /* max probes before drop */

// Exponential backoff multipliers for RTO and Persist timers
extern const int tcp_backoff[];

/**
 * @brief Slow timer thread
 */
void *utcp_slowtimo_thread(void *arg);

#endif
