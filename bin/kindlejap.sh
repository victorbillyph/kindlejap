#!/bin/sh
# KindleJap - KUAL Extension Entry Point

EXTDIR="$(dirname "$0")/.."
cd "$EXTDIR" || exit 1

LOGFILE="/tmp/kindlejap.log"
echo "$(date) KindleJap starting" > "$LOGFILE"

# Clear old app log
rm -f /mnt/us/kindlejap.log 2>/dev/null

# Kill any previous instance
killall kindlejap-bin 2>/dev/null
sleep 0.5

# Prevent screensaver
lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

# Stop Kindle UI services
initctl stop lab126_gui 2>/dev/null
echo "$(date) lab126_gui stopped" >> "$LOGFILE"

initctl stop otaupd 2>/dev/null
initctl stop phd 2>/dev/null
initctl stop tmd 2>/dev/null
initctl stop todo 2>/dev/null
initctl stop mcsd 2>/dev/null

sleep 0.5

# Launch the binary (no exec so we can log if it crashes)
echo "$(date) launching binary" >> "$LOGFILE"
./bin/kindlejap-bin 2>>"$LOGFILE"
RET=$?
echo "$(date) binary exited with code $RET" >> "$LOGFILE"

# Restore Kindle UI
echo "$(date) restoring UI" >> "$LOGFILE"
initctl start lab126_gui 2>/dev/null
initctl start otaupd 2>/dev/null
initctl start phd 2>/dev/null
initctl start tmd 2>/dev/null
initctl start todo 2>/dev/null
initctl start mcsd 2>/dev/null
lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
echo "$(date) done" >> "$LOGFILE"
