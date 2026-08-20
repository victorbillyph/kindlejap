# Changelog

All notable changes to KindleJap will be documented in this file.

## [3.0.0] - 2026-08-20

### Added
- Complete browser rewrite with full HTML support
- **Inline styles**: `<b>`, `<strong>`, `<i>`, `<em>`, `<u>`, `<s>`, `<del>`, `<font color>`, `<span style>`
- **Headings**: h1-h6 rendered at appropriate scales (3x, 2x, 1x) with bold
- **Tables**: `<table>`, `<tr>`, `<td>`, `<th>` rendered with borders and rows
- **Forms**: `<input type="text">`, `<input type="submit">`, `<button>` with tap interaction
- **Images**: `<img alt="...">` shown as placeholder with alt text
- **Lists**: `<ul>` with bullet points, `<ol>` with numbered items
- **Blockquotes**: `<blockquote>` with left border indicator
- **Preformatted text**: `<pre>` with monospace-style background
- **Smart word-wrap**: breaks at word boundaries instead of mid-word
- **Scroll indicator**: visual scroll bar on right edge
- **HTML entities**: 30+ entities including arrows, quotes, currency, copyright
- **Numeric entities**: `&#123;` and `&#x1B;` support
- **Inline CSS**: `color`, `font-weight`, `font-style`, `text-decoration` via `<span style>`
- **Style state tracking**: bold/italic/underline/color inherit through nested tags
- **Tab handling**: tabs converted to spaces
- **Better URL bar**: shows cursor when active, visual feedback

## [2.6.2] - 2026-08-20

### Improved
- Browser now renders structured HTML: headings (h1/h2/h3) in larger fonts
- Paragraph spacing between blocks
- List items with bullet prefix
- Horizontal rules as lines
- Links shown in gray with underline
- `<style>`, `<script>`, `<pre>` properly skipped
- Proper HTML entity support (&mdash;, &hellip;, &rsquo;, etc.)
- Fixed tag parser to handle attributes correctly

## [2.6.1] - 2026-08-20

### Fixed
- Browser segfault caused by broken word-wrap logic (memmove with negative size)
- Fixed style/script tag detection

## [2.6.0] - 2026-08-20

### Added
- Functional web browser with HTML rendering
- Fetches pages via curl with text/html output
- Strips HTML tags to plain text for e-ink display
- Parses `<a href>` links — tappable to navigate
- Links shown underlined in blue
- Scroll up/down with arrow buttons
- Supports relative and absolute URLs
- Handles HTML entities (&amp; &lt; &gt; etc.)
- Ignores `<style>`, `<script>`, `<pre>` sections

## [2.5.3] - 2026-08-20

### Fixed
- Keyboard keys now draw single characters instead of remaining string

## [2.5.2] - 2026-08-20

### Added
- Symbols mode on keyboard (.,!?@#$%&*-+=/\\|~();:'"<>^)
- Toggle between abc / ABC / #123 with mode button

## [2.5.1] - 2026-08-20

### Fixed
- Keyboard: dedicated special row at top (Shift, Backspace, Enter)
- Keyboard: full-width Space bar at bottom
- Keyboard: increased height for proper button spacing
- Topbar: WiFi, notification badge, and battery properly spaced

## [2.5.0] - 2026-08-20

### Changed
- Kindle UI now pauses (SIGSTOP) instead of being killed on startup
- Kindle UI resumes (SIGCONT) on exit — returns to exact state it was in
- Falls back to initctl stop if PID can't be found

## [2.4.0] - 2026-08-20

### Added
- Notification system — topbar shows [!] icon with badge count
- Tap notification icon to open sidebar with all notifications
- Notifications support action buttons
- Tap notification or action to dismiss
- Clear All button in sidebar
- `notif_add()` API for apps to create notifications

## [2.3.0] - 2026-08-20

### Added
- Package Manager app — install/uninstall external apps from GitHub repos
- Install by typing `user/repo` — downloads manifest + binary from latest release
- Uninstall external apps with one tap
- Built-in apps (Calculator, Files, Network, Browser, Package Manager) cannot be uninstalled
- Installed apps tracked in `data/installed.cfg`
- External apps stored in `apps/` directory

### Changed
- Menu now has 7 items (added Package Manager)

## [2.2.0] - 2026-08-20

### Added
- Persistent data storage (`data/` folder) for settings and app state
- Auto-save every 30 seconds + on exit
- Restores last active app, file explorer path, browser URL on startup
- `settings.cfg` and `appstate.cfg` files in data directory

### Fixed
- All centered text now renders inside its correct area (keyboard, calculator, menu, downbar, update dialog, browser)
- Replaced broken `draw_text_centered` with `draw_text_centered_in(x,y,w,...)`

### Changed
- Removed bottom taskbar — full screen space for apps
- Topbar shows WiFi + battery
- Power button opens downbar with Menu / Sleep / Exit
- Menu accessible only through power button downbar

## [2.1.0] - 2026-08-19

### Fixed
- Proper 8x13 bitmap font - digits now render correctly (no longer shown as letters)
- Screen only redraws on touch release (eliminated flicker)

### Added
- Always-visible topbar showing WiFi status and battery percentage
- Power button opens downbar dropdown (Menu, Sleep, Exit)
- Separate input device reading for power button (max77696-onkey)
- Menu opens apps: Calculator, Files, Network, Browser, Check Update
- Rounded rectangle UI elements throughout
- BMP image viewer (File Explorer opens .bmp files)
- PGM image viewer (File Explorer opens .pgm files)
- Clean white desktop background
- MXCFB dual driver support (K51/PW3 Carta + Zelda/PW4+) with auto-detection
- Calculator, File Explorer, Network, and Browser apps

### Changed
- Removed bottom taskbar - full screen space now used by apps
- Power button no longer opens legacy power menu; opens downbar instead
- Menu accessible only through power button downbar

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
