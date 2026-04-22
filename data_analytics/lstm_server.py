#!/usr/bin/env python3
"""
Real-time LSTM congestion prediction server.

Listens on a Unix domain socket for raw CSV rows from the C TCP stack,
maintains a sliding window of time bins, runs LSTM inference on each
completed bin, and sends the congestion-probability back to C as 1 byte.

Usage:
    # Stateless (original) — 7-feature, 15 ms bins, 200-bin window:
    python3 lstm_server.py --model /models/lstm.keras

    # 3M stateless — 8-feature, 5 ms bins, 300-bin window (training model):
    python3 lstm_server.py --model /models/utcp_3m_stateless.keras --mode lstm3m

Wire format:
  C -> Python : one CSV line per ACK event (21 columns, no zlog_ts prefix), newline-terminated
  Python -> C : 1 byte — 0x01 if model fired (p >= threshold), 0x00 otherwise
"""

import socket
import os
import sys
import argparse
import time
import numpy as np
from collections import deque

# ── Per-mode config ───────────────────────────────────────────────────────────
STATELESS_CFG = dict(bin_size_ms=15, seq_len=200, n_features=7)
LSTM3M_CFG    = dict(bin_size_ms=5,  seq_len=300, n_features=8)

# Shared constants
STATELESS_COOLDOWN = 200   # bins of silence after firing (200 × 15 ms = 3 s)
LSTM3M_COOLDOWN    = 600   # bins of silence after firing (600 ×  5 ms = 3 s)

# Column order on the wire (matches logger.c format string — no zlog_ts)
RAW_COLS = [
    'timestamp_us', 'rtt_us', 'srtt_us', 'rttvar_us', 'rto_us', 'min_rtt_us',
    'queue_delay_us', 'rtt_delta_us', 'rtt_accel_us', 'rto_delta_us',
    'cwnd', 'ssthresh', 'snd_wnd', 'flight_size', 'newly_acked',
    'inter_ack_us', 'ca_state', 't_dupacks', 't_rxtshift', 'is_dup_ack', 'is_timeout',
]
N_COLS = len(RAW_COLS)
CI = {c: i for i, c in enumerate(RAW_COLS)}


def parse_row(line: str):
    """Return list of floats or None on bad input."""
    parts = line.strip().split(',')
    if len(parts) != N_COLS:
        return None
    try:
        return [float(p) for p in parts]
    except ValueError:
        return None


class BinAggregator:
    """
    Accumulates per-ACK rows into time bins, normalises features exactly as
    the training notebook does, and maintains a ring buffer of completed bins.

    cfg must contain: bin_size_ms, seq_len, n_features.
      Stateless: seq_len=200, n_features=7 (no rtt_us)
      LSTM3M:    seq_len=300, n_features=8 (rtt_us prepended)
    """

    def __init__(self, cfg: dict):
        self._bin_size_ms = cfg['bin_size_ms']
        self._seq_len     = cfg['seq_len']
        self._n_features  = cfg['n_features']

        self._bins     = np.zeros((self._seq_len, self._n_features), dtype=np.float32)
        self._has_cong = np.zeros(self._seq_len, dtype=bool)
        self._head  = 0
        self._count = 0

        self._cur_bin_id     = None
        self._cur_rows       = []
        self._last_bin_ts_us = 0

        # Median of min_rtt_us samples — mirrors training: run_min_rtt = df['min_rtt_us'].median().
        # A bounded deque avoids unbounded growth; 100k samples covers ~100 s at 1 kHz ACK rate.
        self._min_rtt_vals = deque(maxlen=100_000)
        self._norm_rtt = 1.0

    # ── public ───────────────────────────────────────────────────────────────

    def push(self, vals: list) -> int:
        """
        Add one ACK event row. Returns the number of new bins completed:
        0 if still in the same bin, 1 for a normal crossing, >1 if bin IDs
        were skipped (gap bins filled with zero vectors to mirror the
        linear-interpolation / fillna(0) done during training).
        """
        min_rtt = vals[CI['min_rtt_us']]
        if min_rtt > 0:
            self._min_rtt_vals.append(min_rtt)

        ts_us = vals[CI['timestamp_us']]
        bid   = int(ts_us / (self._bin_size_ms * 1000))

        if self._cur_bin_id is None:
            self._cur_bin_id = bid

        if bid == self._cur_bin_id:
            self._cur_rows.append(vals)
            return 0

        self._finalise()

        # Fill any skipped bin IDs with zero vectors (mirrors training interpolation).
        gap        = max(0, bid - self._cur_bin_id - 1)
        capped_gap = min(gap, self._seq_len)
        for _ in range(capped_gap):
            self._bins[self._head]     = np.zeros(self._n_features, dtype=np.float32)
            self._has_cong[self._head] = False
            self._head  = (self._head + 1) % self._seq_len
            self._count += 1

        self._cur_bin_id = bid
        self._cur_rows   = [vals]
        return 1 + capped_gap

    def ready(self) -> bool:
        """True once at least seq_len bins have been completed."""
        return self._count >= self._seq_len

    def get_sequence(self) -> np.ndarray:
        """Return (seq_len, n_features) in chronological order."""
        order = [(self._head + i) % self._seq_len for i in range(self._seq_len)]
        return self._bins[order]

    def window_has_congestion(self) -> bool:
        """True if any bin in the current window contains a congestion event."""
        return bool(self._has_cong.any())

    def latest_bin_has_congestion(self) -> bool:
        """True if the most recently completed bin has a congestion event."""
        idx = (self._head - 1) % self._seq_len
        return bool(self._has_cong[idx])

    @property
    def last_bin_ts_us(self) -> int:
        """Start timestamp (µs) of the most recently finalised bin."""
        return self._last_bin_ts_us

    # ── private ──────────────────────────────────────────────────────────────

    def _finalise(self):
        rows = self._cur_rows
        self._last_bin_ts_us = int(self._cur_bin_id * self._bin_size_ms * 1000)
        if not rows:
            return

        if self._min_rtt_vals:
            self._norm_rtt = float(np.median(self._min_rtt_vals))
        norm = self._norm_rtt if self._norm_rtt > 0 else 1.0

        def _mean(key):
            idx = CI[key]
            return sum(r[idx] for r in rows) / len(rows)

        def _max(key):
            idx = CI[key]
            return max(r[idx] for r in rows)

        # Per-row ratio then average — matches training notebook exactly.
        def _row_util(r):
            eff = max(1.0, min(r[CI['cwnd']], r[CI['snd_wnd']]))
            return max(0.0, min(2.0, r[CI['flight_size']] / eff))
        utilization = sum(_row_util(r) for r in rows) / len(rows)

        if self._n_features == 8:
            vec = np.array([
                np.clip(_mean('rtt_us')         / norm, -10.0, 10.0),
                np.clip(_mean('srtt_us')        / norm, -10.0, 10.0),
                np.clip(_mean('rttvar_us')      / norm, -10.0, 10.0),
                np.clip(_max ('queue_delay_us') / norm, -10.0, 10.0),
                np.clip(_mean('rtt_delta_us')   / norm, -10.0, 10.0),
                np.clip(_mean('rtt_accel_us')   / norm, -10.0, 10.0),
                np.clip(_max ('t_dupacks')      / 3.0,  -10.0, 10.0),
                np.clip(utilization,                    -10.0, 10.0),
            ], dtype=np.float32)
        else:
            vec = np.array([
                np.clip(_mean('srtt_us')        / norm, -10.0, 10.0),
                np.clip(_mean('rttvar_us')      / norm, -10.0, 10.0),
                np.clip(_max ('queue_delay_us') / norm, -10.0, 10.0),
                np.clip(_mean('rtt_delta_us')   / norm, -10.0, 10.0),
                np.clip(_mean('rtt_accel_us')   / norm, -10.0, 10.0),
                np.clip(_max ('t_dupacks')      / 3.0,  -10.0, 10.0),
                np.clip(utilization,                    -10.0, 10.0),
            ], dtype=np.float32)

        is_cong = any(
            r[CI['ca_state']] >= 3 or r[CI['is_timeout']] >= 1
            for r in rows
        )
        self._bins[self._head]     = vec
        self._has_cong[self._head] = is_cong
        self._head  = (self._head + 1) % self._seq_len
        self._count += 1


# ── Main server loop ─────────────────────────────────────────────────────────

def serve(model_path: str, sock_path: str, threshold: float, cfg: dict, mode: str,
          prob_log: str | None = None):
    import tensorflow as tf

    print(f'Loading model: {model_path}', flush=True)
    model = tf.keras.models.load_model(model_path, compile=False)
    print(f'Model loaded. input_shape={model.input_shape}', flush=True)

    # Compile a fixed-signature inference function once so TF traces the graph
    # before the socket opens. Without this every call re-enters Python dispatch
    # (~1-3 s on CPU); after tracing steady-state drops to single-digit ms.
    n_feat = cfg['n_features']
    seq_len = cfg['seq_len']

    @tf.function(input_signature=[
        tf.TensorSpec(shape=(1, seq_len, n_feat), dtype=tf.float32)
    ])
    def infer(x):
        return model(x, training=False)

    print('Warming up inference function (tracing + 3 calls)...', flush=True)
    _dummy = tf.zeros((1, seq_len, n_feat), dtype=tf.float32)
    for _ in range(3):
        t0 = time.perf_counter()
        infer(_dummy)
        print(f'  warmup: {(time.perf_counter()-t0)*1000:.1f} ms', flush=True)
    print('Warmup done.', flush=True)

    cooldown_len = LSTM3M_COOLDOWN if mode == 'lstm3m' else STATELESS_COOLDOWN

    if os.path.exists(sock_path):
        os.unlink(sock_path)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(1)
    print(f'Listening on {sock_path}  [{mode}]', flush=True)

    while True:
        conn, _ = server.accept()
        print('C client connected.', flush=True)

        agg       = BinAggregator(cfg)
        recv_buf  = ''
        last_byte = b'\x00'
        cooldown  = 0
        rows_ok   = 0
        rows_bad  = 0
        bins_total = 0

        prob_log_f = None
        if prob_log:
            os.makedirs(os.path.dirname(prob_log), exist_ok=True)
            prob_log_f = open(prob_log, 'w')
            prob_log_f.write('bin_start_us,probability,is_congestion,fired,infer_ms\n')

        try:
            conn.settimeout(10.0)
            while True:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    try:
                        conn.sendall(last_byte)
                    except OSError:
                        break
                    continue

                if not chunk:
                    break

                recv_buf += chunk.decode('ascii', errors='replace')

                while '\n' in recv_buf:
                    line, recv_buf = recv_buf.split('\n', 1)
                    vals = parse_row(line)
                    if vals is None:
                        rows_bad += 1
                        if rows_bad <= 3:
                            print(f'[BAD ROW] cols={len(line.split(","))}: {line[:80]}',
                                  flush=True)
                        continue

                    rows_ok    += 1
                    bins_added  = agg.push(vals)
                    bins_total += bins_added

                    if bins_added > 0 and agg.ready():
                        seq = agg.get_sequence()
                        x   = seq[np.newaxis, ...]

                        # Feature diagnostics: print stats every 300 predictions.
                        if bins_total % 300 == 0:
                            feat_names = (
                                ['rtt_us','srtt_us','rttvar_us','queue_delay_us',
                                 'rtt_delta_us','rtt_accel_us','t_dupacks','utilization']
                                if mode == 'lstm3m' else
                                ['srtt_us','rttvar_us','queue_delay_us',
                                 'rtt_delta_us','rtt_accel_us','t_dupacks','utilization']
                            )
                            last = seq[-1]
                            stats = '  '.join(
                                f'{n}={last[i]:.3f}' for i, n in enumerate(feat_names)
                            )
                            print(f'[FEAT] norm_rtt(median)={agg._norm_rtt:.0f}µs  last_bin: {stats}',
                                  flush=True)

                        t0 = time.perf_counter()
                        if mode == 'lstm3m':
                            p = float(infer(x)[0, -1, 0])
                        else:
                            p = float(infer(x)[0, 0])
                        infer_ms = (time.perf_counter() - t0) * 1000.0
                        if infer_ms > 5.0 or bins_total % 300 == 0:
                            print(f'[INFER] {infer_ms:.1f} ms', flush=True)

                        is_cong = agg.window_has_congestion()
                        fired   = False

                        if cooldown > 0:
                            cooldown = max(0, cooldown - bins_added)
                            last_byte = b'\x00'
                        elif is_cong:
                            last_byte = b'\x00'
                        elif p >= threshold:
                            fired     = True
                            cooldown  = cooldown_len
                            last_byte = b'\x01'
                            print(f'[FIRED] p={p:.3f}', flush=True)
                        else:
                            last_byte = b'\x00'

                        if prob_log_f:
                            prob_log_f.write(
                                f'{agg.last_bin_ts_us},{p:.6f},'
                                f'{int(is_cong)},{int(fired)},{infer_ms:.3f}\n'
                            )
                            prob_log_f.flush()

                    try:
                        conn.sendall(last_byte)
                    except OSError:
                        break

        except Exception as exc:
            print(f'Connection error: {exc}', flush=True)
        finally:
            if prob_log_f:
                prob_log_f.close()

        print(f'C client disconnected. rows_ok={rows_ok} rows_bad={rows_bad} '
              f'bins={bins_total}', flush=True)


def main():
    ap = argparse.ArgumentParser(description='UTCP LSTM inference server')
    ap.add_argument('--model',     default='models/utcp_3m_stateless.keras',
                    help='Path to trained .keras model file')
    ap.add_argument('--socket',    default='/tmp/utcp_lstm.sock',
                    help='Unix domain socket path')
    ap.add_argument('--threshold', type=float, default=0.3,
                    help='Probability above which the model fires')
    ap.add_argument('--mode',      choices=['stateless', 'lstm3m'], default='lstm3m',
                    help='stateless: 7-feat 15ms 200-bin; lstm3m: 8-feat 5ms 300-bin')
    ap.add_argument('--prob-log',  default='log/data/lstm_probs.csv',
                    help='CSV file to log per-bin probabilities')
    args = ap.parse_args()

    if not os.path.exists(args.model):
        print(f'ERROR: model not found at {args.model}', file=sys.stderr)
        sys.exit(1)

    cfg = LSTM3M_CFG if args.mode == 'lstm3m' else STATELESS_CFG
    serve(args.model, args.socket, args.threshold, cfg, args.mode,
          prob_log=args.prob_log)


if __name__ == '__main__':
    main()
