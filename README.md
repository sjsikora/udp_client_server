# LSTM-Driven Predictive Congestion Control

Sam Sikora in combination with Alex Taschuk

## Overview

This project investigates whether a machine learning model can predict TCP congestion events *before* they happen, enabling a congestion control algorithm (CCA) to act preemptively rather than reactively.

Classical CCAs like Tahoe, Reno, and NewReno are fundamentally reactive, they wait for packet loss (triple duplicate ACK or timeout) as a signal that the network is saturated, then cut their congestion window. This project asks: can an LSTM learn the subtle warning signs that appear in RTT, queue delay, and duplicate-ACK patterns in the ~2 RTTs before a congestion event fires?

To answer this, the project delivers two things:

1. **UTCP** — a fully custom, user-space TCP implementation over UDP, designed from the ground up with structured logging and modular congestion control to generate the training data an ML model requires.
2. **LSTM Congestion Predictor** — a three-layer LSTM trained on 252 real 1 GB transfers, deployed as a live inference server that communicates with the UTCP stack over a Unix domain socket and outputs a preemptive congestion signal.

[Read the full report here.](https://github.com/sjsikora/udp_client_server/blob/main/Final%20Report.pdf)

## Repository Structure

```
udp_client_server/
├── src/                        # C application code
│   ├── client.c                # Client: reads 1GB file, sends via UTCP
│   ├── server.c                # Server: receives file, writes to disk
│   ├── logging.c               # zlog initialization (3 CSV streams)
│   └── utils.c                 # Error handling, safe print utilities
├── include/
│   └── utcp/
│       ├── api.h               # Public UTCP socket API
│       ├── net/tcp.h           # TCB struct, TCP state machine, segment types
│       ├── config.h            # MSS, buffer sizes, port constants
│       └── cc/
│           ├── core.h          # AIMD primitives, timeout logic
│           ├── tahoe.h         # TCP Tahoe
│           ├── reno.h          # TCP Reno
│           ├── new_reno.h      # TCP NewReno (RFC 6582)
│           ├── lstm_reno.h     # NewReno + LSTM preemptive signal
│           ├── lstm_client.h   # IPC to Python inference server
│           └── logger.h        # log_lstm_event() — writes CSV rows
├── data_analytics/
│   ├── lstm_server.py          # Python LSTM inference server (387 lines)
│   ├── utcp_lstm.ipynb         # Model training notebook
│   └── single_run_analytics.ipynb  # Post-run visualization
├── models/
│   ├── utcp_3m_stateless.keras     # Heavyweight model (48 MB, 3-layer LSTM)
│   ├── lstm_stateless.keras        # Lightweight baseline model (1.5 MB)
├── util/
│   ├── run_once.sh             # Single end-to-end test run
│   ├── collect.sh              # Continuous data collection loop
│   ├── run_client.sh           # Build and run client locally
│   ├── run_server.sh           # Build and run server locally
│   └── transmit.sh             # SSH helper for remote server
├── log/
│   ├── data/                   # CSVs from the current run
│   └── runs/                   # Archived runs (timestamped directories)
├── zlog.conf                   # Log format and output routing
├── Makefile
└── Final Report.pdf
```

---

## UTCP — User-Space TCP over UDP

### Motivation

The User Datagram Protocol provides no delivery guarantees — data arrives out of order, gets dropped, and has no flow or congestion control. This makes it a perfect proxy for raw IP. UTCP mirrors how the kernel embeds TCP segments in IP packets, instead embedding them inside UDP packets. This adds 8 bytes of UDP header overhead per packet and grants full control over the TCP implementation in user space.

A subtlety of this approach is that TCP port numbers inside the segment lose their conventional meaning (the outer UDP header already routes the packet to the correct process port). UTCP repurposes these inner port numbers to implement virtual, user-space ports, enabling multiple concurrent connections to be multiplexed through a single underlying UDP port (`1970`). Each logical connection is assigned a Transmission Control Block (TCB) that tracks all per-connection state: sequence numbers, congestion window, timers, CC algorithm pointer, etc.

### API

UTCP exposes a Berkeley Sockets-compatible API for easy integration:

| Function | Description |
|---|---|
| `utcp_socket()` | Create a UTCP socket; returns a virtual file descriptor |
| `utcp_bind(fd, addr)` | Bind a local address and virtual port to the socket |
| `utcp_connect(fd, addr)` | Establish connection via TCP three-way handshake (blocking) |
| `utcp_listen(fd)` | Mark socket as passive (ready to accept) |
| `utcp_accept(fd)` | Block until an incoming connection is established |
| `utcp_send(fd, buf, len)` | Write data into the UTCP send buffer |
| `utcp_read(fd, buf, len)` | Read incoming data from the receive buffer |
| `utcp_drain(fd)` | Block until all sent data is acknowledged |

### System Architecture

UTCP uses a multi-threaded architecture with three concurrent threads:

- **Main Thread** — executes the application and calls UTCP API functions
- **Listen Thread** — background thread that reads incoming UDP packets, demultiplexes by 4-tuple `(src_ip, src_port, dst_ip, dst_port)`, and dispatches to the matching TCB
- **Timer Thread** — wakes at fixed intervals to decrement per-TCB timers and trigger expiration logic (retransmissions, RTO events)

Any of the three threads may invoke `utcp_output`, which evaluates the current TCB state and sends zero, one, or many segments. The `TF_ACKNOW` flag forces at least one ACK on RFC-required transmissions.

### Implemented TCP Features

- Three-way handshake (SYN, SYN-ACK, ACK)
- TCP finite state machine
- Cumulative and delayed acknowledgments
- Out-of-order (OOO) segment reassembly
- Retransmission timer (RTO) per RFC 6298
- Flow control (receive window)
- Congestion control: TCP Tahoe, Reno, NewReno, and LSTM-Reno
- Slow start and congestion avoidance
- Fast retransmit and fast recovery
- Window scaling

### Modular Congestion Control

The TCB holds a pointer to a swappable CC function. On any congestion event (normal ACK, duplicate ACK, timeout), this function is invoked with the event type and arguments. The CC updates `cwnd` and `ssthresh`; `utcp_output` enforces that bytes in flight never exceed the window.
