#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

HOST="0.0.0.0"
PORT="8080"

# ── Parse arguments ──
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--host HOST] [--port PORT]"; exit 1 ;;
    esac
done

# ── Build if needed ──
if [[ ! -f "$BUILD_DIR/dbserver" ]] || [[ ! -f "$BUILD_DIR/dbcli" ]]; then
    echo "[*] Build not found, running build.sh..."
    bash "$SCRIPT_DIR/build.sh"
fi

echo "=== Starting databasetopit ==="
echo "[*] Server: $HOST:$PORT"

# ── Start server in background ──
"$BUILD_DIR/dbserver" --host "$HOST" --port "$PORT" &
SERVER_PID=$!
sleep 1

# ── Check server started ──
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[!] Server failed to start"
    exit 1
fi

echo "[✓] Server running (PID $SERVER_PID)"
echo "[*] Starting CLI client..."
echo ""

# ── Start CLI ──
"$BUILD_DIR/dbcli" --host "$HOST" --port "$PORT" || true

# ── Cleanup ──
echo ""
echo "[*] Shutting down server..."
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
echo "[✓] Done."
