#!/bin/bash

SERVER_USER="dmrocks"
SERVER_IP="40.82.162.155"
SERVER_KEY="/Users/samsikora/Desktop/Desktop/UBC/4.2/COSC_448/udp_client_server/mac-laptop.pem"
SERVER_DIR="udp_client_server"
SERVER_PORT=1970
RUN=0

while true; do
    rm -f log/data/*.csv

    RUN=$((RUN + 1))
    echo "======== Run $RUN ========"

    echo "[1/4] Building client..."
    make

    echo "[2/4] Starting server..."
    ssh -i "$SERVER_KEY" "$SERVER_USER@$SERVER_IP" \
        "sudo fuser -k ${SERVER_PORT}/udp 2>/dev/null; cd $SERVER_DIR && make && time ./build/server" \
        > log/server.log 2>&1 &
    SERVER_PID=$!

    # Give the server time to build and start listening
    sleep 5

    echo "[3/4] Starting client..."
    ./build/client

    echo "[3/4] Client done, waiting for server..."
    wait $SERVER_PID

    echo "[4/4] Saving CSVs..."
    FOLDER="log/runs/$(date +%m_%d_%H_%M_%S)"
    mkdir -p "$FOLDER"
    cp log/data/*.csv "$FOLDER/" 2>/dev/null
    rm -f log/data/*.csv

    echo "Run $RUN saved to $FOLDER"

    RUNS_SIZE=$(du -sk log/runs | awk '{print $1}')
    if [ "$RUNS_SIZE" -gt $((10 * 1024 * 1024)) ]; then
        echo "log/runs has exceeded 10GB (${RUNS_SIZE}KB). Stopping."
        exit 0
    fi

    echo "Sleeping 10s before next run..."
    sleep 10
done
