# Changelog

All notable changes to KindleJap will be documented in this file.

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
