/**
 * @brief TCP LSTM-Reno CC Algorithm
 *
 * New Reno (RFC 6582) with a preemptive ssthresh reduction triggered by
 * the Python LSTM inference server when it predicts imminent congestion.
 */
extern const struct tcp_congestion_ops utcp_lstm_reno;
