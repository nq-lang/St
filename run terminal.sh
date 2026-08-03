#!/usr/bin/env bash
# run_terminal.sh
#
# Launches the built terminal against the codespace's virtual display
# (provided by the devcontainer's desktop-lite feature, which runs Xvfb on
# :1 and exposes it via noVNC on forwarded port 6080). Open the "Terminal
# Display (noVNC)" forwarded port in your browser, then run this script
# from the VS Code terminal — the window will appear in that browser tab.
set -euo pipefail

export DISPLAY="${DISPLAY:-:1}"

BIN="./build/qqq_terminal"
if [ ! -x "$BIN" ]; then
    echo "Binary not found at $BIN — building first..."
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"
fi

echo ">>> Launching on DISPLAY=$DISPLAY"
echo ">>> View it via the forwarded 'Terminal Display (noVNC)' port (6080) in your browser."
exec "$BIN"
