# Changelog

All notable changes to KindleJap will be documented in this file.

## [2.1.0] - 2026-08-19

### Fixed
- Proper 8x13 bitmap font - digits now render correctly (no longer shown as letters)

### Added
- Rounded rectangle UI elements throughout
- Mini popup start menu (swipe down from taskbar menu button)
- Mini popup power menu (swipe down from taskbar power button)
- Power button on taskbar
- BMP image viewer (File Explorer opens .bmp files)
- PGM image viewer (File Explorer opens .pgm files)
- Clean white desktop background
- MXCFB dual driver support (K51/PW3 Carta + Zelda/PW4+) with auto-detection
- Calculator, File Explorer, Network, and Browser apps

## [2.0.0] - 2026-08-19

### Added
- Auto-update with restart - checks GitHub on startup and via menu
- Shows update dialog, downloads and installs, then execs new binary
- App framework with maximize mode, app stack, and registry
- QWERTY keyboard overlay with shift, backspace, and submit
- Calculator app with basic operations
- File Explorer app with directory browsing
- Network Manager app with WiFi scanning and connection
- Browser app with URL input and basic text rendering
- Taskbar with open app tabs
- Long-press taskbar tab to close apps
- KUAL app integration (scans /mnt/us/extensions/)
- Community apps support (/mnt/us/kindlejap_apps/)
- Improved splash screen with progress bar

### Changed
- Complete rewrite of kindlejap.c as single-file application
- Menu now lists all built-in apps directly

## [1.2.0] - 2026-08-19

### Fixed
- Properly hide Kindle UI using `initctl stop lab126_gui` and related services
- Fix touch input by scanning all `/dev/input/event*` devices
- Use `setsid` in menu.json to survive framework shutdown

### Added
- Splash screen with app name centered on startup
- Initialization status display (testing framebuffer, input, updates)
- Kill previous instance before starting (lock file mechanism)
- Restore Kindle UI on exit
- Disable screensaver while app is running

## [1.1.2] - 2026-08-19

### Fixed
- Changed extension ID from `japlat` to `kindlejap` to match folder name
- Updated all hardcoded paths to use `kindlejap` folder
- Fixed release workflow to package as `kindlejap/` instead of `japlat/`
- Updated README and Makefile with correct folder name

## [1.1.1] - 2026-08-19

### Fixed
- KUAL integration: replaced extension.json with proper config.xml and menu.json
- Shell script paths corrected for KUAL execution
- Release packaging now creates correct folder structure

## [1.1.0] - 2026-08-19

### Added
- Auto-update system with GitHub integration
- Check for updates on startup
- Update dialog with user confirmation
- Manual "Check Update" option in menu
- Version comparison logic
- Update download and install script

## [1.0.0] - 2026-08-19

### Added
- Initial release of KindleJap launcher
- Full-screen interface with framebuffer GUI
- Taskbar at bottom of screen
- Swipe down gesture to reveal menu
- Menu button on taskbar
- Menu with Apps, Settings, and Exit options
- Exit button returns to Kindle UI
- KUAL extension configuration
- Makefile for ARM cross-compilation
- Touch input handling via Linux input subsystem

### Known Issues
- Apps menu not yet functional
- Settings menu not yet functional
- Basic font (5x7 pixels) - no high-res typography yet
