#!/usr/bin/env bash
# run_mmo_server.sh — Start the SparkEngine MMO headless server for testing.
#
# Usage:
#   ./Tests/scripts/run_mmo_server.sh [start|stop|status]
#
# The PID is stored in /tmp/spark_mmo_server.pid for lifecycle management.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
ENGINE="${BUILD_DIR}/bin/SparkEngine"
MODULE="${BUILD_DIR}/bin/libSparkGameMMO.so"
PID_FILE="/tmp/spark_mmo_server.pid"
LOG_FILE="/tmp/spark_mmo_server.log"

usage() {
    echo "Usage: $0 [start|stop|status]"
    echo "  start   — launch the headless MMO server (port 27015)"
    echo "  stop    — kill a previously started server"
    echo "  status  — check whether the server is running"
    exit 1
}

do_start() {
    if [[ -f "$PID_FILE" ]]; then
        local old_pid
        old_pid=$(cat "$PID_FILE")
        if kill -0 "$old_pid" 2>/dev/null; then
            echo "Server already running (PID $old_pid)"
            return 0
        fi
        rm -f "$PID_FILE"
    fi

    if [[ ! -x "$ENGINE" ]]; then
        echo "ERROR: Engine not found at $ENGINE — run cmake --build build first"
        exit 1
    fi
    if [[ ! -f "$MODULE" ]]; then
        echo "ERROR: MMO module not found at $MODULE"
        exit 1
    fi

    echo "Starting SparkEngine MMO headless server..."
    "$ENGINE" -headless -game "$MODULE" > "$LOG_FILE" 2>&1 &
    local pid=$!
    echo "$pid" > "$PID_FILE"

    # Wait for the server to bind its socket
    for i in $(seq 1 20); do
        sleep 0.25
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "ERROR: Server exited early. Check $LOG_FILE"
            rm -f "$PID_FILE"
            exit 1
        fi
        if grep -q "6987" /proc/"$pid"/net/udp 2>/dev/null; then
            echo "Server started (PID $pid, port 27015)"
            echo "Log: $LOG_FILE"
            return 0
        fi
    done

    echo "WARNING: Server started (PID $pid) but port 27015 not yet visible"
}

do_stop() {
    if [[ ! -f "$PID_FILE" ]]; then
        echo "No PID file — server not running"
        return 0
    fi
    local pid
    pid=$(cat "$PID_FILE")
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
        echo "Server stopped (was PID $pid)"
    else
        echo "Server not running (stale PID $pid)"
    fi
    rm -f "$PID_FILE"
}

do_status() {
    if [[ ! -f "$PID_FILE" ]]; then
        echo "Not running (no PID file)"
        return 1
    fi
    local pid
    pid=$(cat "$PID_FILE")
    if kill -0 "$pid" 2>/dev/null; then
        echo "Running (PID $pid)"
        return 0
    else
        echo "Not running (stale PID $pid)"
        rm -f "$PID_FILE"
        return 1
    fi
}

case "${1:-start}" in
    start)  do_start ;;
    stop)   do_stop  ;;
    status) do_status ;;
    *)      usage ;;
esac
