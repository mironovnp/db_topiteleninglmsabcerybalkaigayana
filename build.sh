#!/usr/bin/env bash
set -euo pipefail

echo "=== databasetopit build ==="

# ── Detect OS & package manager ──
install_deps() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if command -v apt-get &>/dev/null; then
            sudo apt-get update -qq
            sudo apt-get install -y -qq cmake g++ make git libssl-dev
        elif command -v dnf &>/dev/null; then
            sudo dnf install -y cmake gcc-c++ make git openssl-devel
        elif command -v pacman &>/dev/null; then
            sudo pacman -Sy --noconfirm cmake gcc make git openssl
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        if command -v brew &>/dev/null; then
            brew install cmake git openssl
        fi
    fi
}

# Check for cmake
if ! command -v cmake &>/dev/null; then
    echo "[*] Installing dependencies..."
    install_deps
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[*] Configuring with CMake..."
cmake "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release

echo "[*] Building..."
cmake --build . --parallel "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

echo "[✓] Build complete. Binaries in $BUILD_DIR"

# echo "=== Building C# GUI ==="
# if command -v dotnet &>/dev/null; then
#     echo "[*] Building CaseChampGui..."
#     cd "$SCRIPT_DIR/CaseChampGui"
#     dotnet build
#     echo "[✓] GUI Build complete."
# else
#     echo "[!] 'dotnet' command not found. Skipping C# GUI build."
#     echo "    To build the GUI, please install the .NET SDK (https://dotnet.microsoft.com/download)."
# fi
