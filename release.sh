#!/usr/bin/env bash
# Build firmware, copy to Pi's OTA dir, bump version.txt.
# Next time the tracker boots within home WiFi range, it picks up the update.
#
# Prereqs:
#   - PlatformIO installed locally
#   - SSH access to the Pi (host alias `traccar` or set $PI_HOST)
#   - Firmware FW_VERSION constant in main.cpp bumped to match the new version
#
# Usage:  ./release.sh <new_version>
#   e.g.  ./release.sh 2

set -euo pipefail
NEW_VER="${1:?usage: $0 <new_version_int>}"
PI_HOST="${PI_HOST:-traccar}"
PI_FW_DIR="${PI_FW_DIR:-/opt/tracker/ota}"

cd "$(dirname "$0")/firmware"

# Sanity check: source FW_VERSION should match
grep -q "FW_VERSION   = ${NEW_VER};" src/main.cpp \
  || { echo "ERROR: src/main.cpp does not set FW_VERSION = ${NEW_VER}"; exit 1; }

echo "[release] building firmware v${NEW_VER}..."
pio run

BIN=".pio/build/t-sim7000g/firmware.bin"
[[ -f "$BIN" ]] || { echo "ERROR: build artifact not found"; exit 1; }

echo "[release] copying to ${PI_HOST}:${PI_FW_DIR}/firmware.bin..."
scp "$BIN" "${PI_HOST}:/tmp/firmware.bin"
ssh "$PI_HOST" "sudo mv /tmp/firmware.bin ${PI_FW_DIR}/firmware.bin && \
                echo ${NEW_VER} | sudo tee ${PI_FW_DIR}/version.txt > /dev/null && \
                sudo systemctl restart ota-server"

echo "[release] done. Tracker will pick up v${NEW_VER} on next wake within home WiFi."
echo "          tail logs on Pi:  ssh ${PI_HOST} 'sudo journalctl -u ota-server -f'"
