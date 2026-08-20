# KindleJap - Kindle Launcher

A custom launcher for Kindle devices, launched via KUAL (Kindle Unified Application Launcher).

## Features

- Full-screen interface that takes over from Kindle UI
- Taskbar at the bottom of the screen
- Swipe down from bottom to reveal menu
- Menu with Apps, Settings, Check Update, and Exit options
- **Auto-update**: Checks GitHub for new versions and updates automatically
- Exit button returns to Kindle UI

## Installation

1. Install KUAL on your Kindle device
2. Download the latest release from [GitHub Releases](https://github.com/victorbillyph/kindlejap/releases)
3. Extract the `japlat` folder to `/mnt/us/extensions/` on your Kindle
4. The structure should be: `/mnt/us/extensions/japlat/config.xml`
5. Restart KUAL or refresh extensions
6. Launch "KindleJap" from the KUAL menu

### Manual Installation

If you prefer to build from source:

1. Clone this repository
2. Run `make` to cross-compile for ARM
3. Copy the entire `japlat` folder to `/mnt/us/extensions/` on your Kindle

## Building

### Prerequisites

- ARM cross-compiler (`arm-linux-gnueabi-gcc`)
- Make

### Build Commands

```bash
# Build for Kindle (ARM)
make

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
   - **Check Update**: Manually check for updates from GitHub
   - **Exit**: Return to Kindle UI

### Auto-Update

KindleJap automatically checks for updates on startup. If a new version is available:
- A dialog will appear asking if you want to update
- Tap **UPDATE** to download and install the latest version
- The app will restart after updating

You can also manually check for updates from the menu.

## Project Structure

```
japlat/
├── config.xml              # KUAL extension manifest
├── menu.json               # KUAL menu definition
├── bin/
│   ├── kindlejap-bin       # Main executable (ARM)
│   ├── kindlejap.sh        # KUAL launch script
│   └── update.sh           # Update script
├── src/
│   └── kindlejap.c         # Main application source
├── Makefile                # Build system
├── CHANGELOG.md            # Version history
└── README.md               # This file
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
- [x] Auto-update system
- [x] Check for updates on startup
- [x] Update dialog with user confirmation
- [x] Proper KUAL integration (config.xml + menu.json)
- [ ] Apps launcher functionality
- [ ] Settings persistence
- [ ] App icons and better typography
- [ ] Additional input methods

## License

MIT License - See LICENSE file for details
