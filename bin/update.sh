#!/bin/sh
# KindleJap Update Script
# Usage: update.sh [current_version] [download_url] or update.sh restart

SCRIPT_DIR="$(dirname "$0")"
EXTENSION_DIR="/mnt/us/extensions/kindlejap"
TEMP_DIR="/tmp/kindlejap_update"
DOWNLOAD_URL="https://github.com/victorbillyph/kindlejap/archive/refs/heads/master.zip"

# If restart argument, just re-execute the launcher
if [ "$1" = "restart" ]; then
    exec "$SCRIPT_DIR/kindlejap-bin"
fi

CURRENT_VERSION="${1:-0.0.0}"
DOWNLOAD_URL="${2:-$DOWNLOAD_URL}"

echo "KindleJap Updater"
echo "Current version: $CURRENT_VERSION"
echo "Downloading from: $DOWNLOAD_URL"

# Create temp directory
mkdir -p "$TEMP_DIR"
cd "$TEMP_DIR"

# Download the update
echo "Downloading update..."
curl -L -o kindlejap.zip "$DOWNLOAD_URL" 2>/dev/null
if [ $? -ne 0 ]; then
    echo "Failed to download update"
    rm -rf "$TEMP_DIR"
    exit 1
fi

# Extract
echo "Extracting..."
unzip -q -o kindlejap.zip
if [ $? -ne 0 ]; then
    echo "Failed to extract update"
    rm -rf "$TEMP_DIR"
    exit 1
fi

# Find the extracted directory
EXTRACTED_DIR=$(find . -maxdepth 1 -type d -name "kindlejap-*" | head -n 1)
if [ -z "$EXTRACTED_DIR" ]; then
    EXTRACTED_DIR=$(find . -maxdepth 1 -type d -name "japlat-*" | head -n 1)
fi

if [ -z "$EXTRACTED_DIR" ]; then
    echo "Failed to find extracted directory"
    rm -rf "$TEMP_DIR"
    exit 1
fi

echo "Found: $EXTRACTED_DIR"

# Backup current version
echo "Backing up current version..."
cp -r "$EXTENSION_DIR/bin" "$EXTENSION_DIR/bin.bak" 2>/dev/null

# Copy new files
echo "Installing update..."
cp "$EXTRACTED_DIR/bin/kindlejap-bin" "$EXTENSION_DIR/bin/" 2>/dev/null
cp "$EXTRACTED_DIR/bin/kindlejap.sh" "$EXTENSION_DIR/bin/" 2>/dev/null
cp "$EXTRACTED_DIR/bin/update.sh" "$EXTENSION_DIR/bin/" 2>/dev/null

# Set permissions
chmod +x "$EXTENSION_DIR/bin/kindlejap-bin"
chmod +x "$EXTENSION_DIR/bin/kindlejap.sh"
chmod +x "$EXTENSION_DIR/bin/update.sh"

# Clean up
echo "Cleaning up..."
rm -rf "$TEMP_DIR"

echo "Update complete!"
echo "Restarting KindleJap..."
exec "$EXTENSION_DIR/bin/kindlejap.sh"
