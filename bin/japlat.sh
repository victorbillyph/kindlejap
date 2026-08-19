#!/bin/sh
# KindleJap - KUAL Extension Entry Point
# This script is called by KUAL to start the launcher

SCRIPT_DIR="$(dirname "$0")"
cd "$SCRIPT_DIR"

# Stop Kindle UI services to take over the screen
/usr/bin/lipc-set-prop com.lab126.winbroker appSwitcherDisable true 2>/dev/null

# Kill any existing instances
killall kindlejap-bin 2>/dev/null

# Start the launcher
./bin/kindlejap-bin

# When the launcher exits, restore Kindle UI
/usr/bin/lipc-set-prop com.lab126.winbroker appSwitcherDisable false 2>/dev/null
