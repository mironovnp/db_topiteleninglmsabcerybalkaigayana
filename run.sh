#!/usr/bin/env bash
# CaseChamp one-command launcher.
# Builds the C++ engine if needed, then starts the GUI.
# The GUI itself launches build/dbserver if it is not already running.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

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

GUI_DIR="$ROOT_DIR/CaseChampGui"
if [ "$SKIP_GUI_BUILD" -eq 0 ]; then
    echo "[CaseChamp] Restoring GUI packages (NuGet)..."
    dotnet restore "$GUI_DIR/CaseChampGui.csproj" -nologo -v minimal
    echo "[CaseChamp] Building GUI..."
    dotnet build "$GUI_DIR/CaseChampGui.csproj" -c Debug -nologo -v minimal --no-restore
fi

echo "[CaseChamp] Launching GUI..."
exec dotnet run --project "$GUI_DIR/CaseChampGui.csproj" -c Debug --no-build --no-restore
