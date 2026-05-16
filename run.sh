#!/usr/bin/env bash
# CaseChamp one-command launcher.
# Builds the C++ engine if needed, then starts the GUI.
# The GUI itself launches build/dbserver if it is not already running.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

export AVALONIA_TELEMETRY_OPTOUT="${AVALONIA_TELEMETRY_OPTOUT:-1}"

SKIP_BUILD=0
SKIP_GUI_BUILD=0
RELEASE=0
for arg in "$@"; do
    case "$arg" in
        --no-build) SKIP_BUILD=1 ;;
        --no-gui-build) SKIP_GUI_BUILD=1 ;;
        --release) RELEASE=1 ;;
        -h|--help)
            cat <<EOF
Usage: ./run.sh [--no-build] [--no-gui-build] [--release]

  --no-build      Skip C++ (cmake) build step.
  --no-gui-build  Skip dotnet build step (use cached binaries).
  --release       Build C++ in Release mode.

GUI restore and compile are separate steps so a long NuGet restore is visible
and does not restart MSBuild from scratch. If restore hangs, check proxy/VPN
and run: dotnet restore CaseChampGui/CaseChampGui.csproj -v:n
EOF
            exit 0
            ;;
    esac
done

if [ "$SKIP_BUILD" -eq 0 ]; then
    BUILD_TYPE="Debug"
    [ "$RELEASE" -eq 1 ] && BUILD_TYPE="Release"
    echo "[CaseChamp] Building C++ engine ($BUILD_TYPE)..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null
    cmake --build build -j
fi

if [ ! -x "build/dbserver" ]; then
    echo "[CaseChamp] WARNING: build/dbserver not found. GUI will start without the server."
fi

DATA_DIR="${CASECHAMP_DATA_DIR:-$ROOT_DIR/data}"
mkdir -p "$DATA_DIR"
export CASECHAMP_DATA_DIR="$DATA_DIR"

start_dbserver_if_needed() {
    [ -x "build/dbserver" ] || return 0
    if curl -sf "http://127.0.0.1:8080/ping" >/dev/null 2>&1; then
        if pgrep -f "$ROOT_DIR/build/dbserver" >/dev/null 2>&1; then
            echo "[CaseChamp] Перезапуск dbserver (data: $DATA_DIR)..."
            pkill -f "$ROOT_DIR/build/dbserver" 2>/dev/null || true
            sleep 0.4
        else
            echo "[CaseChamp] WARNING: порт 8080 занят не нашим dbserver. БД могут быть не из $DATA_DIR"
            return 0
        fi
    fi
    echo "[CaseChamp] Starting dbserver on 0.0.0.0:8080 (data: $DATA_DIR)..."
    ./build/dbserver --host 0.0.0.0 --port 8080 --data-dir "$DATA_DIR" &
    DB_PID=$!
    for _ in $(seq 1 40); do
        if curl -sf "http://127.0.0.1:8080/ping" >/dev/null 2>&1; then
            echo "[CaseChamp] dbserver ready (pid $DB_PID)."
            return 0
        fi
        if ! kill -0 "$DB_PID" 2>/dev/null; then
            echo "[CaseChamp] ERROR: dbserver exited before /ping succeeded."
            return 1
        fi
        sleep 0.25
    done
    echo "[CaseChamp] WARNING: dbserver did not respond on /ping in time."
    return 1
}

start_dbserver_if_needed || true

ensure_dotnet() {
    if command -v dotnet >/dev/null 2>&1; then
        return 0
    fi

    DOTNET_HOME="$HOME/.dotnet"
    if [ -x "$DOTNET_HOME/dotnet" ]; then
        export PATH="$DOTNET_HOME:$DOTNET_HOME/tools:$PATH"
        export DOTNET_ROOT="$DOTNET_HOME"
        return 0
    fi

    echo "[CaseChamp] .NET SDK not found. Installing automatically..."

    local urls=(
        "https://dot.net/v1/dotnet-install.sh"
        "https://dotnet.microsoft.com/download/dotnet/scripts/v1/dotnet-install.sh"
        "https://dotnetcli.azureedge.net/dotnet/scripts/v1/dotnet-install.sh"
    )

    local success=0
    for url in "${urls[@]}"; do
        echo "[CaseChamp] Attempting to download install script from $url..."
        if curl -sSL --connect-timeout 10 --retry 3 "$url" -o dotnet-install.sh; then
            success=1
            break
        fi
    done

    if [ "$success" -eq 0 ]; then
        if [ -f "dotnet-install.sh" ]; then
            echo "[CaseChamp] Using existing dotnet-install.sh"
        else
            echo "[CaseChamp] ERROR: Failed to download dotnet-install.sh."
            exit 1
        fi
    fi

    bash ./dotnet-install.sh --version latest --install-dir "$DOTNET_HOME"
    export PATH="$DOTNET_HOME:$DOTNET_HOME/tools:$PATH"
    export DOTNET_ROOT="$DOTNET_HOME"
}

ensure_dotnet

GUI_DIR="$ROOT_DIR/CaseChampGui"
GUI_BIN="$GUI_DIR/bin/Debug/net10.0/CaseChampGui"
if [ "$SKIP_GUI_BUILD" -eq 0 ]; then
    echo "[CaseChamp] Building GUI (restore + compile)..."
    dotnet build "$GUI_DIR/CaseChampGui.csproj" -c Debug -nologo -v minimal
fi

if [ ! -x "$GUI_BIN" ] && [ ! -f "$GUI_BIN.dll" ]; then
    echo "[CaseChamp] ERROR: GUI binary not found: $GUI_BIN"
    exit 1
fi

echo "[CaseChamp] Launching GUI..."

if [ -z "${DISPLAY:-}" ] && grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then
    export DISPLAY=:0
    echo "[CaseChamp] DISPLAY не был задан — использую DISPLAY=:0 (WSLg)."
fi
if [ -n "${DISPLAY:-}" ] && ! xdpyinfo >/dev/null 2>&1; then
    echo "[CaseChamp] WARNING: DISPLAY=$DISPLAY недоступен. Запустите WSLg или VcXsrv."
fi

export AVALONIA_X11_USE_GPU=0
export LIBGL_ALWAYS_SOFTWARE=1
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-disable}"
unset WAYLAND_DISPLAY

GUI_LOG="${CASECHAMP_GUI_LOG:-$ROOT_DIR/.casechamp-gui.log}"

launch_gui() {
    pkill -f "$ROOT_DIR/CaseChampGui/bin/" 2>/dev/null || true
    sleep 0.2

    : >"$GUI_LOG"
    if [ -x "$GUI_BIN" ]; then
        "$GUI_BIN" >>"$GUI_LOG" 2>&1 &
    else
        dotnet exec "$GUI_DIR/bin/Debug/net10.0/CaseChampGui.dll" >>"$GUI_LOG" 2>&1 &
    fi
    GUI_PID=$!
    echo "$GUI_PID" >"$ROOT_DIR/.casechamp-gui.pid"
}

verify_gui_started() {
    sleep 1
    if ! kill -0 "$GUI_PID" 2>/dev/null; then
        echo "[CaseChamp] ERROR: GUI завершился сразу после запуска (pid $GUI_PID)."
        if [ -s "$GUI_LOG" ]; then
            echo "[CaseChamp] Лог GUI ($GUI_LOG):"
            tail -n 40 "$GUI_LOG"
        fi
        return 1
    fi
    return 0
}

if grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then
    launch_gui
    if ! verify_gui_started; then
        exit 1
    fi
    echo "[CaseChamp] GUI запущен (pid $GUI_PID). Сначала «Загрузка…», затем основное окно."
    echo "[CaseChamp] Лог: $GUI_LOG"
    exit 0
fi

if [ -x "$GUI_BIN" ]; then
    exec "$GUI_BIN"
else
    exec dotnet exec "$GUI_DIR/bin/Debug/net10.0/CaseChampGui.dll"
fi
