#!/usr/bin/env python3
"""
Real-time LSTM congestion prediction server.

Listens on a Unix domain socket for raw CSV rows from the C TCP stack,
maintains a sliding window of time bins, runs LSTM inference on each
completed bin, and sends the congestion-probability back to C as 1 byte.

Usage:
    # Stateless (original) — 7-feature, 15 ms bins, 200-bin window:
    python3 lstm_server.py --model /models/lstm.keras

    # Stateful (v6) — 8-feature, 5 ms bins, one-bin-at-a-time streaming:
    python3 lstm_server.py --model /models/utcp_stateful_lstm_v6_infer.keras --stateful

Wire format:
  C -> Python : one CSV line per ACK event (21 columns, no zlog_ts prefix), newline-terminated
  Python -> C : 1 byte — 0x01 if model fired (p >= threshold), 0x00 otherwise
"""

import socket
import os
import sys
import argparse
import numpy as np

# ── Per-mode config ───────────────────────────────────────────────────────────
STATELESS_CFG = dict(bin_size_ms=15, seq_len=200, n_features=7)
STATEFUL_CFG  = dict(bin_size_ms=5,  seq_len=1,   n_features=8)

# Shared constants
NORM_WARMUP       = 50    # ACK rows with valid min_rtt to collect before locking baseline
STATELESS_COOLDOWN = 200  # bins of silence after firing (200 × 15 ms = 3 s)
STATEFUL_COOLDOWN  = 600  # bins of silence after firing (600 ×  5 ms = 3 s)

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
      Stateful:  seq_len=1,   n_features=8 (rtt_us prepended)
    """

    def __init__(self, cfg: dict):
        self._bin_size_ms = cfg['bin_size_ms']
        self._seq_len     = cfg['seq_len']
        self._n_features  = cfg['n_features']

        self._bins     = np.zeros((self._seq_len, self._n_features), dtype=np.float32)
        self._has_cong = np.zeros(self._seq_len, dtype=bool)
        self._head  = 0
        self._count = 0

        self._cur_bin_id = None
        self._cur_rows   = []

        # Per-connection RTT normalisation baseline.
        self._norm_rtt         = None
        self._norm_rtt_samples = []

    # ── public ───────────────────────────────────────────────────────────────

    def push(self, vals: list) -> bool:
        """
        Add one ACK event row. Returns True when a bin boundary is crossed
        (a new bin has been appended to the window).
        """
        min_rtt = vals[CI['min_rtt_us']]
        if min_rtt > 0 and self._norm_rtt is None:
            self._norm_rtt_samples.append(min_rtt)
            if len(self._norm_rtt_samples) >= NORM_WARMUP:
                self._norm_rtt = float(np.median(self._norm_rtt_samples))

        ts_us = vals[CI['timestamp_us']]
        bid   = int(ts_us / (self._bin_size_ms * 1000))

        if self._cur_bin_id is None:
            self._cur_bin_id = bid

        if bid == self._cur_bin_id:
            self._cur_rows.append(vals)
            return False

        self._finalise()
        self._cur_bin_id = bid
        self._cur_rows   = [vals]
        return True

    def ready(self) -> bool:
        """True once at least seq_len bins have been completed."""
        return self._count >= self._seq_len

    def get_sequence(self) -> np.ndarray:
        """Return (seq_len, n_features) in chronological order (stateless use)."""
        order = [(self._head + i) % self._seq_len for i in range(self._seq_len)]
        return self._bins[order]

    def get_latest_bin(self) -> np.ndarray:
        """Return (1, 1, n_features) — latest completed bin (stateful use)."""
        idx = (self._head - 1) % self._seq_len
        return self._bins[idx: idx + 1][np.newaxis, ...]

    def window_has_congestion(self) -> bool:
        """True if any bin in the current window contains a congestion event (stateless)."""
        return bool(self._has_cong.any())

    def latest_bin_has_congestion(self) -> bool:
        """True if the most recently completed bin has a congestion event (stateful)."""
        idx = (self._head - 1) % self._seq_len
        return bool(self._has_cong[idx])

    # ── private ──────────────────────────────────────────────────────────────

    def _finalise(self):
        rows = self._cur_rows
        if not rows:
            return

        norm = self._norm_rtt if (self._norm_rtt and self._norm_rtt > 0) else 1.0

        def _mean(key):
            idx = CI[key]
            return sum(r[idx] for r in rows) / len(rows)

        def _max(key):
            idx = CI[key]
            return max(r[idx] for r in rows)

        eff_win     = max(1.0, min(_mean('cwnd'), _mean('snd_wnd')))
        utilization = max(0.0, min(2.0, _mean('flight_size') / eff_win))

        if self._n_features == 8:
            # Stateful v6: rtt_us prepended as first feature
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
            # Stateless: 7-feature order (no rtt_us)
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

def serve(model_path: str, sock_path: str, threshold: float, cfg: dict, stateful: bool):
    import tensorflow as tf

    print(f'Loading model: {model_path}', flush=True)
    model = tf.keras.models.load_model(model_path, compile=False)
    print(f'Model loaded. input_shape={model.input_shape}', flush=True)

    cooldown_len = STATEFUL_COOLDOWN if stateful else STATELESS_COOLDOWN

    if os.path.exists(sock_path):
        os.unlink(sock_path)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(1)
    print(f'Listening on {sock_path}  [{"stateful" if stateful else "stateless"}]', flush=True)

    while True:
        conn, _ = server.accept()
        print('C client connected.', flush=True)

        if stateful:
            model.reset_states()

        agg       = BinAggregator(cfg)
        recv_buf  = ''
        last_byte = b'\x00'
        cooldown  = 0

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
                        continue

                    bin_crossed = agg.push(vals)

                    if bin_crossed and agg.ready():
                        if stateful:
                            # Always feed the latest bin to keep hidden state current.
                            x = agg.get_latest_bin()           # (1, 1, 8)
                            p = float(model.predict(x, verbose=0)[0, 0, 0])
                            if cooldown > 0:
                                cooldown -= 1
                                last_byte = b'\x00'
                            elif agg.latest_bin_has_congestion():
                                last_byte = b'\x00'
                            else:
                                if p >= threshold:
                                    cooldown  = cooldown_len
                                    last_byte = b'\x01'
                                    print(f'[FIRED] p={p:.3f}', flush=True)
                                else:
                                    last_byte = b'\x00'
                        else:
                            if cooldown > 0:
                                cooldown -= 1
                                last_byte = b'\x00'
                            elif agg.window_has_congestion():
                                last_byte = b'\x00'
                            else:
                                seq = agg.get_sequence()        # (200, 7)
                                x   = seq[np.newaxis, ...]      # (1, 200, 7)
                                p   = float(model.predict(x, verbose=0)[0, 0])
                                if p >= threshold:
                                    cooldown  = cooldown_len
                                    last_byte = b'\x01'
                                    print(f'[FIRED] p={p:.3f}', flush=True)
                                else:
                                    last_byte = b'\x00'

                    try:
                        conn.sendall(last_byte)
                    except OSError:
                        break

        except Exception as exc:
            print(f'Connection error: {exc}', flush=True)

        print('C client disconnected.', flush=True)


def main():
    ap = argparse.ArgumentParser(description='UTCP LSTM inference server')
    ap.add_argument('--model',     default='models/lstm_stateful.keras',
                    help='Path to trained .keras model file')
    ap.add_argument('--socket',    default='/tmp/utcp_lstm.sock',
                    help='Unix domain socket path')
    ap.add_argument('--threshold', type=float, default=0.39085638523101807,
                    help='Probability above which the model fires')
    ap.add_argument('--stateful',  action='store_true',
                    help='Use stateful LSTM inference model '
                         '(v6 infer model: 8 features, 5 ms bins, one-bin-at-a-time)')
    args = ap.parse_args()

    if not os.path.exists(args.model):
        print(f'ERROR: model not found at {args.model}', file=sys.stderr)
        sys.exit(1)

    cfg = STATEFUL_CFG if args.stateful else STATELESS_CFG
    serve(args.model, args.socket, args.threshold, cfg, args.stateful)


if __name__ == '__main__':
    main()
