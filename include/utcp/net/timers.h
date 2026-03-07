#ifndef TIMERS_H
#define TIMERS_H

struct tcb;

/* In ms, how long is the slow tick timer. Down from 500ms */
#define TCP_TICK_MS 10

/* Helper macros to convert real time into your tick system */
#define MS_TO_TICKS(ms)   ((ms) / TCP_TICK_MS)
#define SEC_TO_TICKS(sec) (((sec) * 1000) / TCP_TICK_MS)

/* Modernized TCP timer constants defined in ticks */
#define TCPTV_MSL SEC_TO_TICKS(30) /* 30 seconds max seg lifetime */

#define TCPTV_MIN      MS_TO_TICKS(200) /* 200ms minimum RTO (Standard modern TCP) */
#define TCPTV_REXMTMAX SEC_TO_TICKS(64) /* 64 seconds max RTO */

#define TCPTV_PERSMIN SEC_TO_TICKS(5)  /* 5 seconds retransmit persistence */
#define TCPTV_PERSMAX SEC_TO_TICKS(60) /* 60 seconds maximum persist interval */

#define TCPTV_KEEP_INIT SEC_TO_TICKS(75)   /* 75 seconds initial connect keepalive */
#define TCPTV_KEEP_IDLE SEC_TO_TICKS(7200) /* 2 hours dflt time before probing */
#define TCPTV_KEEPINTVL SEC_TO_TICKS(75)   /* 75 seconds default probe interval */

#define TCPTV_SRTTBASE 0                 /* base roundtrip time */
#define TCPTV_SRTTDFLT MS_TO_TICKS(1000) /* 1 second assumed RTO if no info (RFC 6298) */

#define TCP_LINGERTIME  SEC_TO_TICKS(120) /* linger at most 2 minutes */
#define TCP_MAXRXTSHIFT 12                /* maximum retransmits */
#define TCPTV_KEEPCNT   8                 /* max probes before drop */

// Exponential backoff multipliers for RTO and Persist timers
extern const int tcp_backoff[];

/**
 * @brief Slow timer thread
 */
void *utcp_slowtimo_thread(void *arg);

void utcp_xmit_timer(struct tcb *tcb, int rtt_ticks);

#endif
