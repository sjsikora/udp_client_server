# UTCP — UDP-based TCP Implementation

## LSTM Training Data: `log/data/lstm_output.csv`

One row is written per ACK event (valid ACK, duplicate ACK, or RTO timeout). Normalization and look-ahead labeling are done offline in Python before training.

| # | Column | C type | Unit | Description |
|---|---|---|---|---|
| 1 | `timestamp_us` | `uint64_t` | µs | Absolute monotonic clock time at the moment of logging. Not used as an LSTM feature — use `inter_ack_us` instead. |
| 2 | `rtt_us` | `uint32_t` | µs | Instantaneous RTT measured for this ACK. **0 on dup-ACK and timeout rows** (no sample available). |
| 3 | `srtt_us` | `uint32_t` | µs | Smoothed RTT from the Jacobson/Karels EWMA (`t_srtt >> 3`, converted from ticks). Lags the instantaneous RTT. |
| 4 | `rttvar_us` | `uint32_t` | µs | RTT variance from the EWMA (`t_rttvar >> 2`, converted from ticks). Grows when RTT becomes inconsistent — a key pre-congestion signal. |
| 5 | `rto_us` | `uint32_t` | µs | Current retransmission timeout (`t_rxtcur * TCP_TICK_MS * 1000`). Equal to `SRTT + 4·RTTVAR`. The primary anomaly signal — rises before loss occurs. |
| 6 | `min_rtt_us` | `uint64_t` | µs | Running minimum RTT seen on this connection (updated only on Karn-valid samples). Used as the baseline for normalization offline. |
| 7 | `queue_delay_us` | `int64_t` | µs | `rtt_us - min_rtt_us`. Estimated queue occupancy. 0 on dup-ACK/timeout rows or before `min_rtt_us` is established. Never negative (clamped). |
| 8 | `rtt_delta_us` | `int32_t` | µs | First derivative of RTT: `rtt_us[t] - rtt_us[t-1]`. Positive means latency is growing. 0 when no valid RTT sample this row. |
| 9 | `rtt_accel_us` | `int32_t` | µs | Second derivative of RTT: `rtt_delta[t] - rtt_delta[t-1]`. Positive means the growth is accelerating. 0 when no valid RTT sample this row. |
| 10 | `rto_delta_us` | `int32_t` | µs | First derivative of RTO: `rto_us[t] - rto_us[t-1]`. Captures combined SRTT + RTTVAR growth in a single value. Updated every row. |
| 11 | `cwnd` | `uint32_t` | bytes | Congestion window at the moment of logging (post-CC-callback, so reflects any fast-retransmit or timeout reduction). |
| 12 | `ssthresh` | `uint32_t` | bytes | Slow start threshold. Clamped to 1,050,000 (750 × MSS) to prevent the enormous initial value (`0xFFFFFFFF`) from corrupting training. |
| 13 | `snd_wnd` | `uint32_t` | bytes | Receiver-advertised window. Effective send window is `min(cwnd, snd_wnd)`. |
| 14 | `flight_size` | `uint32_t` | bytes | Bytes in-flight: `snd_nxt - snd_una`. For timeout rows, read before `snd_nxt` rollback so this reflects the true in-flight data at loss. |
| 15 | `newly_acked` | `uint32_t` | bytes | Bytes acknowledged by this ACK (`ack - old_snd_una`). 0 on dup-ACK and timeout rows. |
| 16 | `inter_ack_us` | `uint64_t` | µs | Time since the previous logged event on this connection. Acts as the LSTM's clock. 0 for the very first event. |
| 17 | `ca_state` | `uint32_t` | enum | Congestion state: `0`=OPEN, `1`=DISORDER, `2`=CWR, `3`=RECOVERY, `4`=LOSS. One-hot encode before feeding to the LSTM. |
| 18 | `t_dupacks` | `uint32_t` | count | Consecutive duplicate ACKs received so far. Resets to 0 on any valid ACK. Fast retransmit fires at 3. |
| 19 | `t_rxtshift` | `uint32_t` | count | Number of consecutive RTO expirations (exponential backoff counter). 0 in normal operation; >0 means repeated timeout. |
| 20 | `is_dup_ack` | `uint32_t` | bool (0/1) | 1 if this row was logged from the duplicate-ACK path. `rtt_us`, `queue_delay_us`, `rtt_delta_us`, `rtt_accel_us`, and `newly_acked` will all be 0. |
| 21 | `is_timeout` | `uint32_t` | bool (0/1) | 1 if this row was logged from an RTO expiry. `rtt_us` is 0. `flight_size` reflects pre-rollback in-flight data. `cwnd`/`ssthresh` reflect pre-CC values (the state that caused the loss). |
