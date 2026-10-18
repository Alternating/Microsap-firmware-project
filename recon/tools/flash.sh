#!/bin/bash
# CLI reflash for the Midwest Mods board — no manual BOOTSEL button.
# Sends P2M_BOOTSEL over the debug UART, waits for the USB bootloader drive, copies the UF2.
#
# usage: ./flash.sh [debug|normal]      (default: debug)
#   PORT=/dev/ttyUSB0 ./flash.sh        (override serial port)
#
# Requirements: debug (UART) firmware running; serial adapter TX wired to the board's GPIO5
# (RX) pad, not just GPIO4; board's USB port connected to this machine for the BOOTSEL drive.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
UF2="$DIR/midwestmods-2350-v1.00-debuguart.uf2"
[ "$1" = "normal" ] && UF2="$DIR/midwestmods-2350-v1.00.uf2"
PORT="${PORT:-/dev/ttyUSB0}"
[ -f "$UF2" ] || { echo "!! $UF2 not found"; exit 1; }

echo "-> sending P2M_BOOTSEL on $PORT"
sudo stty -F "$PORT" 115200 raw -echo
printf '\r\nP2M_BOOTSEL\r\n' | sudo tee "$PORT" >/dev/null

echo "-> waiting for the BOOTSEL drive"
DEV=""
for _ in $(seq 1 40); do
    DEV=$(lsblk -rno NAME,LABEL 2>/dev/null | awk '$2 ~ /RP2350|RPI-RP2/ {print $1; exit}')
    [ -n "$DEV" ] && break
    sleep 0.5
done
if [ -z "$DEV" ]; then
    echo "!! no BOOTSEL drive appeared after 20s. Check that:"
    echo "   - the DEBUG (UART) build is running (normal build has no serial console)"
    echo "   - the adapter's TX is wired to the board's GPIO5 pad (commands go board-ward)"
    echo "   - the board's USB port is plugged into this machine"
    echo "   Fallback: hold BOOTSEL while plugging USB, then re-run this script."
    exit 1
fi

MNT=$(udisksctl mount -b "/dev/$DEV" 2>/dev/null | sed 's/.* at //') || true
if [ -z "$MNT" ]; then
    sudo mkdir -p /mnt/p2m
    sudo mount "/dev/$DEV" /mnt/p2m
    MNT=/mnt/p2m
fi
echo "-> copying $(basename "$UF2") -> $MNT"
sudo cp "$UF2" "$MNT/"
sync
echo "-> done: board reboots into the new firmware now"
