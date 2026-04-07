#!/bin/bash
set -e

SERVER_USER="dmrocks"
SERVER_IP="40.82.162.155"
SERVER_KEY="/Users/samsikora/Desktop/Desktop/UBC/4.2/COSC_448/udp_client_server/mac-laptop.pem"
SERVER_DIR="udp_client_server"
SERVER_PORT=1970
LSTM_MODEL="/Users/samsikora/Desktop/Desktop/UBC/4.2/COSC_448/udp_client_server/models/lstm.keras"
LSTM_SOCK="/tmp/utcp_lstm.sock"

# Kill background processes on exit (clean finish, error, or Ctrl-C)
cleanup() {
    if [ -n "$REMOTE_PID" ]; then
        echo "Stopping remote server (pid $REMOTE_PID)..."
        kill "$REMOTE_PID" 2>/dev/null
        wait "$REMOTE_PID" 2>/dev/null
    fi
    if [ -n "$LSTM_PID" ]; then
        echo "Stopping LSTM server (pid $LSTM_PID)..."
        kill "$LSTM_PID" 2>/dev/null
        # Give Python/TF up to 5s to exit gracefully, then force kill
        for i in $(seq 1 5); do
            kill -0 "$LSTM_PID" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$LSTM_PID" 2>/dev/null
        wait "$LSTM_PID" 2>/dev/null
        rm -f "$LSTM_SOCK"
    fi
}
trap cleanup EXIT

echo "[1/4] Clearing previous run data..."
rm -f log/data/*.csv

echo "[2/4] Building client..."
make

echo "[3/5] Starting LSTM inference server..."
rm -f "$LSTM_SOCK"
source data_analytics/.venv/bin/activate
python3 data_analytics/lstm_server.py --model "$LSTM_MODEL" --socket "$LSTM_SOCK" \
    > log/lstm_server.log 2>&1 &
LSTM_PID=$!

# Wait until the Python server creates the socket file (TF load can be slow)
echo "Waiting for LSTM server to be ready..."
for i in $(seq 1 60); do
    [ -S "$LSTM_SOCK" ] && { echo "LSTM server ready (${i}s)."; break; }
    if [ "$i" -eq 60 ]; then
        echo "Warning: LSTM server socket not found after 60s, continuing anyway."
    fi
    sleep 1
done

echo "[4/5] Starting remote server..."
ssh -i "$SERVER_KEY" "$SERVER_USER@$SERVER_IP" \
    "sudo fuser -k ${SERVER_PORT}/udp 2>/dev/null; cd $SERVER_DIR && make && time ./build/server" \
    > log/server.log 2>&1 &
REMOTE_PID=$!

sleep 5

echo "[5/5] Running client..."
./build/client

echo "Done. Data left in log/data/"
