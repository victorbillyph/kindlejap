# KindleJap - Kindle Launcher

A custom launcher for Kindle devices, launched via KUAL (Kindle Unified Application Launcher).

## Features

- Full-screen interface that takes over from Kindle UI
- Taskbar at the bottom of the screen
- Swipe down from bottom to reveal menu
- Menu with Apps, Settings, and Exit options
- Exit button returns to Kindle UI

## Installation

1. Install KUAL on your Kindle device
2. Copy the entire `japlat` folder to `/mnt/us/extensions/japlat/` on your Kindle
3. Restart KUAL or refresh extensions
4. Launch "KindleJap" from the KUAL menu

## Building

### Prerequisites

- ARM cross-compiler (arm-linux-gnueabi-gcc)
- Make

### Build Commands

```bash
# Build for Kindle (ARM)
make

# Build for development (x86 Linux)
make dev

# Clean build artifacts
make clean
```

## Usage

1. Launch KindleJap from KUAL
2. The launcher will take over the full screen
3. **Swipe down** from the bottom of the screen to open the menu
4. **Tap the MENU button** on the taskbar to toggle the menu
5. Select from:
   - **Apps**: Open installed applications (coming soon)
   - **Settings**: Configure launcher settings (coming soon)
   - **Exit**: Return to Kindle UI

## Project Structure

```
japlat/
├── bin/                    # Compiled binaries and scripts
│   ├── kindlejap-bin      # Main executable
│   └── kindlejap.sh       # KUAL launch script
├── src/
│   └── kindlejap.c        # Main application source
├── lib/                    # Libraries (if needed)
├── extension.json         # KUAL extension configuration
├── Makefile               # Build system
└── README.md              # This file
```

## Technical Details

- Written in C for optimal performance on Kindle hardware
- Direct framebuffer access for GUI rendering
- Multi-touch input handling via Linux input subsystem
- Grayscale rendering optimized for e-ink displays

## Development Status

- [x] Basic framebuffer GUI
- [x] Touch input handling
- [x] Taskbar with menu button
- [x] Menu system (Apps, Settings, Exit)
- [ ] Apps launcher functionality
- [ ] Settings persistence
- [ ] App icons and better typography
- [ ] Additional input methods

## License

MIT License - See LICENSE file for details
