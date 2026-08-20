#!/bin/sh
# KindleJap - KUAL Extension Entry Point
# Hides Kindle UI, kills previous instances, launches launcher

EXTDIR="$(dirname "$0")/.."
cd "$EXTDIR" || exit 1

# Kill any previous instance
killall kindlejap-bin 2>/dev/null
sleep 0.5

# Prevent screensaver
lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

# Stop Kindle UI services (do NOT stop: framework, powerd, lab126)
initctl stop lab126_gui 2>/dev/null
initctl stop otaupd 2>/dev/null
initctl stop phd 2>/dev/null
initctl stop tmd 2>/dev/null
initctl stop todo 2>/dev/null
initctl stop mcsd 2>/dev/null

sleep 0.3

# Launch the binary
exec ./bin/kindlejap-bin
