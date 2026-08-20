# KindleJap Launcher - Makefile for Kindle Cross-Compilation
# Requires Kindle SDK or cross-compiler (arm-linux-gnueabi-gcc)

# Cross-compiler settings (adjust path as needed)
CC = arm-linux-gnueabi-gcc
STRIP = arm-linux-gnueabi-strip

# Compiler flags
CFLAGS = -O2 -Wall -Wextra -static
LDFLAGS = -static

# Source files
SRC_DIR = src
BIN_DIR = bin
SRC_FILES = $(SRC_DIR)/kindlejap.c
BIN_FILE = $(BIN_DIR)/kindlejap-bin

# KUAL extension files
EXTENSION_JSON = extension.json
LAUNCH_SCRIPT = $(BIN_DIR)/kindlejap.sh

# Default target
all: build

# Build the binary
build: $(SRC_FILES)
	@echo "Building KindleJap..."
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_FILE) $(SRC_FILES) $(LDFLAGS)
	$(STRIP) $(BIN_FILE)
	@chmod +x $(BIN_FILE)
	@chmod +x $(LAUNCH_SCRIPT)
	@echo "Build complete: $(BIN_FILE)"

# Clean build artifacts
clean:
	@echo "Cleaning..."
	rm -f $(BIN_FILE)
	@echo "Clean complete"

# Install to Kindle (via USB or SSH)
install: build
	@echo "To install:"
	@echo "1. Create 'extensions/kindlejap' folder on Kindle"
	@echo "2. Copy this entire folder to 'extensions/kindlejap'"
	@echo "3. Restart KUAL or refresh extensions"

# Development target for x86 testing (if on Linux)
dev:
	@echo "Building for development (x86)..."
	@mkdir -p $(BIN_DIR)
	gcc -O2 -Wall -Wextra -o $(BIN_FILE).dev $(SRC_FILES)
	@echo "Build complete: $(BIN_FILE).dev"

.PHONY: all build clean install dev
