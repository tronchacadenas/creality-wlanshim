#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "ERROR: x86_64-w64-mingw32-g++ not found." >&2
    echo "Install via: sudo apt install -y g++-mingw-w64-x86-64" >&2
    exit 1
fi

echo "=== Compiling wlanapi.dll with MinGW-w64 GCC ==="
x86_64-w64-mingw32-g++ -std=c++14 -O2 -shared -static -static-libgcc -static-libstdc++ \
    -o wlanapi.dll wlanapi_shim.cpp wlanapi.def

HASH="$(sha256sum wlanapi.dll | awk '{print $1}')"
echo "=== Build successful! ==="
echo "Output:  ${SCRIPT_DIR}/wlanapi.dll ($(stat -c%s wlanapi.dll) bytes)"
echo "SHA-256: ${HASH}"
