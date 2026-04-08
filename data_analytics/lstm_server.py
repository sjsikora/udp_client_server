#!/usr/bin/env python3
"""
Real-time LSTM congestion prediction server.

Listens on a Unix domain socket for raw CSV rows from the C TCP stack,
maintains a 200-bin (15 ms/bin) sliding window, runs LSTM inference on
each completed bin, and sends the congestion-probability back to C as a
4-byte IEEE-754 float.

Usage:
    python3 lstm_server.py [--model /models/lstm.keras] [--socket /tmp/utcp_lstm.sock]

Wire format:
  C -> Python : one CSV line per ACK event (21 columns, no zlog_ts prefix), newline-terminated
  Python -> C : 1 byte — 0x01 if model fired (p >= threshold), 0x00 otherwise
"""

import socket
import os
import sys
import argparse
import numpy as np

# ── Config (must match training notebook) ────────────────────────────────────
BIN_SIZE_MS  = 15      # ms per time bin
SEQ_LEN      = 200     # 200 bins × 15 ms = 3 s lookback
N_FEATURES   = 7
NORM_WARMUP  = 50      # ACK rows with valid min_rtt to collect before locking the baseline

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
    Accumulates per-ACK rows into BIN_SIZE_MS time bins, normalises features
    exactly as the training notebook does, and maintains a SEQ_LEN-bin deque.
    """

    def __init__(self):
        # Fixed-length ring of completed bin feature vectors
        self._bins     = np.zeros((SEQ_LEN, N_FEATURES), dtype=np.float32)
        self._has_cong = np.zeros(SEQ_LEN, dtype=bool)   # per-bin congestion flag
        self._head  = 0          # next write position (circular)
        self._count = 0          # total bins ever completed

        self._cur_bin_id   = None
        self._cur_rows     = []  # raw parsed rows accumulating in current bin

        # Per-connection RTT normalization baseline.
        # Matches training: collect NORM_WARMUP samples then lock in their
        # median (training used median(min_rtt_us) over the full run).
        self._norm_rtt         = None
        self._norm_rtt_samples = []

    # ── public ───────────────────────────────────────────────────────────────

    def push(self, vals: list) -> bool:
        """
        Add one ACK event row.  Returns True when a bin boundary is crossed
        (and thus a new bin has been appended to the window).
        """
        min_rtt = vals[CI['min_rtt_us']]
        if min_rtt > 0 and self._norm_rtt is None:
            self._norm_rtt_samples.append(min_rtt)
            if len(self._norm_rtt_samples) >= NORM_WARMUP:
                self._norm_rtt = float(np.median(self._norm_rtt_samples))

        ts_us = vals[CI['timestamp_us']]
        bid   = int(ts_us / (BIN_SIZE_MS * 1000))

        if self._cur_bin_id is None:
            self._cur_bin_id = bid

        if bid == self._cur_bin_id:
            self._cur_rows.append(vals)
            return False

        # Bin boundary: finalise current, start new
        self._finalise()
        self._cur_bin_id = bid
        self._cur_rows   = [vals]
        return True

    def ready(self) -> bool:
        """True once SEQ_LEN bins have been completed."""
        return self._count >= SEQ_LEN

    def get_sequence(self) -> np.ndarray:
        """Return (SEQ_LEN, N_FEATURES) in chronological order."""
        order = [(self._head + i) % SEQ_LEN for i in range(SEQ_LEN)]
        return self._bins[order]

    def window_has_congestion(self) -> bool:
        """True if any bin in the current SEQ_LEN window contains a congestion event.
        Matches the training skip rule: windows overlapping ca_state>=3 or is_timeout>=1
        were excluded from training, so the model has never learned to handle them."""
        return bool(self._has_cong.any())

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
        self._head  = (self._head + 1) % SEQ_LEN
        self._count += 1


# ── Main server loop ─────────────────────────────────────────────────────────

def serve(model_path: str, sock_path: str, threshold: float):
    import tensorflow as tf  # deferred so --help works without TF installed

    print(f'Loading model: {model_path}', flush=True)
    # compile=False skips deserialising the saved loss/optimiser config,
    # which is not needed for inference-only use.
    model = tf.keras.models.load_model(model_path, compile=False)
    print('Model loaded.', flush=True)

    if os.path.exists(sock_path):
        os.unlink(sock_path)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(1)
    print(f'Listening on {sock_path}', flush=True)

    while True:
        conn, _ = server.accept()
        print('C client connected.', flush=True)

        agg       = BinAggregator()
        recv_buf  = ''
        last_byte = b'\x00'
        cooldown  = 0   # bins remaining before next inference is allowed

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
                        if cooldown > 0:
                            cooldown -= 1
                            last_byte = b'\x00'
                        elif agg.window_has_congestion():
                            # Window overlaps active recovery — outside training
                            # distribution, suppress prediction.
                            last_byte = b'\x00'
                        else:
                            seq = agg.get_sequence()         # (200, 7)
                            x   = seq[np.newaxis, ...]       # (1, 200, 7)
                            p   = float(model.predict(x, verbose=0)[0, 0])
                            fired = p >= threshold
                            if fired:
                                cooldown  = SEQ_LEN  # silence for 3 s after firing
                                last_byte = b'\x01'
                                print(f'[FIRED] p={p:.3f}', flush=True)
                            else:
                                last_byte = b'\x00'

                    # Send latest fired byte after each row so C stays current
                    try:
                        conn.sendall(last_byte)
                    except OSError:
                        break

        except Exception as exc:
            print(f'Connection error: {exc}', flush=True)

        print('C client disconnected.', flush=True)


def main():
    ap = argparse.ArgumentParser(description='UTCP LSTM inference server')
    ap.add_argument('--model',     default='/models/lstm.keras',
                    help='Path to trained .keras model file')
    ap.add_argument('--socket',    default='/tmp/utcp_lstm.sock',
                    help='Unix domain socket path')
    ap.add_argument('--threshold', type=float, default=0.39085638523101807,
                    help='Probability above which a WARN is printed (default 0.7)')
    args = ap.parse_args()

    if not os.path.exists(args.model):
        print(f'ERROR: model not found at {args.model}', file=sys.stderr)
        sys.exit(1)

    serve(args.model, args.socket, args.threshold)


if __name__ == '__main__':
    main()
