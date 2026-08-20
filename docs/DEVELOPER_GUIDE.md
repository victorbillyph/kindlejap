# KindleJap Developer Guide

Complete documentation for building apps and extensions for the KindleJap launcher.

---

## Table of Contents

1. [Overview](#overview)
2. [App Architecture](#app-architecture)
3. [Drawing API](#drawing-api)
4. [Colors](#colors)
5. [Font System](#font-system)
6. [Screen & Layout](#screen--layout)
7. [Touch Input](#touch-input)
8. [Keyboard](#keyboard)
9. [Notifications](#notifications)
10. [Data Persistence](#data-persistence)
11. [E-Ink Display](#e-ink-display)
12. [Package Manager](#package-manager)
13. [Building & Compiling](#building--compiling)
14. [Complete Example App](#complete-example-app)
15. [Best Practices](#best-practices)

---

## Overview

KindleJap is a custom launcher for Kindle e-readers written in C. It provides a full app framework with:

- Framebuffer-based UI rendering (8-bit grayscale, 1072x1448 pixels)
- Touch input handling
- On-screen keyboard
- Notification system
- Package manager for distributing apps
- E-ink optimized display

Apps are written in C, compiled to static ARM binaries, and run directly on the Kindle hardware.

---

## App Architecture

Every KindleJap app is defined by the `App` struct:

```c
typedef struct {
    const char *name;                              // Display name in menus
    void (*init)(void);                            // Called once when app opens (may be NULL)
    void (*draw)(int x, int y, int w, int h);      // Render app content
    void (*on_touch)(int x, int y, int released);  // Handle touch events
    void (*cleanup)(void);                         // Called when app closes (may be NULL)
} App;
```

### App Lifecycle

| Phase | Function | When |
|-------|----------|------|
| Registration | `app_register(&my_app)` | At startup, before main loop |
| Open | `app_open(&my_app)` | User selects app from menu |
| Running | `draw()` + `on_touch()` | Every frame (20 Hz) while app is active |
| Close | `cleanup()` | User closes app or launcher exits |

### Creating an App

```c
#include <stdio.h>
#include <string.h>

// App state
static int counter = 0;

static void my_app_init(void) {
    counter = 0;
}

static void my_app_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);

    // Title
    draw_text_centered_in(x, y + 20, w, "My Counter App", COLOR_BLACK, 3);

    // Counter value
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", counter);
    draw_text_centered_in(x, y + 100, w, buf, COLOR_BLACK, 5);

    // Buttons
    draw_rounded_rect(x + 100, y + 200, 200, 50, 8, COLOR_DARK);
    draw_text_centered_in(x + 100, y + 210, 200, " + 1 ", COLOR_WHITE, 2);

    draw_rounded_rect(x + 100, y + 280, 200, 50, 8, COLOR_MID);
    draw_text_centered_in(x + 100, y + 290, 200, " Reset ", COLOR_WHITE, 2);
}

static void my_app_handle(int tx, int ty, int released) {
    if (!released) return;

    if (point_in_rect(tx, ty, 100, 200, 200, 50)) {
        counter++;
        dirty = 1;
    }
    if (point_in_rect(tx, ty, 100, 280, 200, 50)) {
        counter = 0;
        dirty = 1;
    }
}

// Register the app
static App my_app = {"Counter", my_app_init, my_app_draw, my_app_handle, NULL};

// In main():
app_register(&my_app);
```

### Registering Built-in Apps

```c
static App calc_app = {"Calculator", NULL, calc_draw, calc_handle, NULL};
static App file_app = {"Files", NULL, file_draw, file_handle, NULL};
static App net_app  = {"Network", net_init, net_draw, net_handle, NULL};
static App browser_app = {"Browser", NULL, browser_draw, browser_handle, NULL};
static App pkg_app  = {"Package Manager", NULL, pkg_draw, pkg_handle, NULL};

app_register(&calc_app);
app_register(&file_app);
app_register(&net_app);
app_register(&browser_app);
app_register(&pkg_app);
```

---

## Drawing API

All drawing functions operate on the framebuffer. Content is only visible after `refresh_screen()` is called (done automatically by the main loop when `dirty = 1`).

### Pixel-Level

```c
void draw_pixel(int x, int y, unsigned char color);
```
Draws a single pixel. Coordinates outside the screen are silently ignored.

### Shapes

```c
void draw_rect(int x, int y, int w, int h, unsigned char color);
```
Fills a rectangle with the given color.

```c
void draw_rounded_rect(int x, int y, int w, int h, int radius, unsigned char color);
```
Fills a rectangle with rounded corners.

```c
void draw_circle(int cx, int cy, int radius, unsigned char color);
```
Fills a circle centered at (cx, cy).

```c
void draw_line(int x0, int y0, int x1, int y1, unsigned char color);
```
Draws a line using Bresenham's algorithm.

### Images

```c
void draw_bmp(const char *path, int x, int y);
```
Loads and renders a 24-bit BMP image. Converts RGB to grayscale automatically.

```c
void draw_pgm(const char *path, int x, int y);
```
Loads and renders a binary PGM (P5) grayscale image.

### Text

```c
void draw_char(int x, int y, char ch, unsigned char color, int scale);
```
Draws a single character at the given position and scale.

```c
void draw_text(int x, int y, const char *text, unsigned char color, int scale);
```
Draws a string of text left-aligned at (x, y).

```c
void draw_text_centered_in(int x, int y, int width, const char *text, unsigned char color, int scale);
```
Draws text horizontally centered within the given width starting at x.

```c
void draw_text_right(int x_right, int y, const char *text, unsigned char color, int scale);
```
Draws text right-aligned to the given x coordinate.

```c
int text_width(const char *text, int scale);
```
Returns the pixel width of the rendered text.

---

## Colors

KindleJap uses 8-bit grayscale. Available color constants:

```c
#define COLOR_WHITE  0xFF   // Pure white (background)
#define COLOR_BLACK  0x00   // Pure black (primary text)
#define COLOR_DARK   0x30   // Dark gray (secondary text, buttons)
#define COLOR_MID    0xA0   // Medium gray (subtle text, borders)
#define COLOR_LIGHT  0xE8   // Light gray (backgrounds, cards)
#define COLOR_LIGHTER 0xD0  // Lighter gray (alternatives)
```

You can use any value from 0 (black) to 255 (white) for custom shades.

---

## Font System

The built-in font is an 8x13 pixel bitmap font supporting ASCII 32-126.

```c
#define FONT_W 8    // Character width in pixels
#define FONT_H 13   // Character height in pixels
```

### Scale System

All text functions accept a `scale` parameter that multiplies the font size:

| Scale | Character Size | Pixels/Char | Use Case |
|-------|---------------|-------------|----------|
| 1 | 8x13 px | 8 px wide | Body text, small labels |
| 2 | 16x26 px | 16 px wide | Headings, buttons |
| 3 | 24x39 px | 24 px wide | Section titles |
| 4 | 32x52 px | 32 px wide | Large titles |
| 5 | 40x65 px | 40 px wide | Hero text |

### Character Count Per Line

On a 1072px wide screen with 10px padding on each side:

| Scale | Characters/Line |
|-------|----------------|
| 1 | ~131 |
| 2 | ~65 |
| 3 | ~43 |
| 4 | ~32 |
| 5 | ~26 |

---

## Screen & Layout

### Screen Dimensions

```c
int screen_width;    // Typically 1072 on Kindle Paperwhite
int screen_height;   // Typically 1448 on Kindle Paperwhite
```

### Layout Constants

```c
#define TOPBAR_H  40    // Height of the status bar (WiFi, battery, notifications)
#define KEYBOARD_H 360  // Height of the on-screen keyboard
```

### App Content Area

Your `draw()` function receives the usable area:

```c
void my_draw(int x, int y, int w, int h) {
    // x = 0
    // y = TOPBAR_H (40)
    // w = screen_width (1072)
    // h = screen_height - TOPBAR_H (1408)
}
```

The top bar (WiFi, battery, notifications) is drawn by KindleJap above your content.

### Keyboard Overlay

When the keyboard is visible, it overlays the bottom 360 pixels of the screen. If your app needs keyboard input, account for this in your layout.

---

## Touch Input

### Event Handling

```c
void my_app_handle(int tx, int ty, int released);
```

- `tx, ty`: Touch coordinates in screen pixels
- `released`: Always 1 when your handler is called (only finger-release events are forwarded)

### Hit Testing

```c
int point_in_rect(int px, int py, int x, int y, int w, int h);
```
Returns 1 if point (px, py) is inside the rectangle. The rectangle is half-open: includes left/top edges, excludes right/bottom.

### Example: Button Detection

```c
void my_app_handle(int tx, int ty, int released) {
    if (!released) return;

    // Check if "Submit" button was tapped
    int btn_x = 100, btn_y = 500, btn_w = 300, btn_h = 50;
    if (point_in_rect(tx, ty, btn_x, btn_y, btn_w, btn_h)) {
        // Button was tapped
        do_submit();
        dirty = 1;  // Request screen redraw
    }
}
```

### Scroll Example

```c
static int scroll = 0;

void my_app_handle(int tx, int ty, int released) {
    if (!released) return;

    // Scroll up button
    if (point_in_rect(tx, ty, screen_width - 50, screen_height - 100, 40, 40)) {
        scroll -= 5;
        if (scroll < 0) scroll = 0;
        dirty = 1;
    }

    // Scroll down button
    if (point_in_rect(tx, ty, 10, screen_height - 100, 40, 40)) {
        scroll += 5;
        dirty = 1;
    }
}
```

### Touch Priority

Touch events pass through this chain before reaching your app:

1. Update dialog (if visible)
2. Setup wizard (if active)
3. On-screen keyboard (if visible)
4. Down bar (power menu)
5. Main menu
6. Notification sidebar
7. Your app's `on_touch()`

---

## Keyboard

KindleJap provides a built-in on-screen keyboard for text input.

### Keyboard State

```c
static int keyboard_visible = 0;     // 1 when keyboard is showing
static int keyboard_mode = 0;        // 0=lowercase, 1=UPPERCASE, 2=symbols
static int keyboard_cursor = 0;      // Cursor position in buffer
static char keyboard_buf[128] = "";  // Input buffer (max 127 chars)
```

### Opening the Keyboard

```c
// Copy initial text into buffer
strcpy(keyboard_buf, "hello");
keyboard_cursor = strlen(keyboard_buf);
keyboard_visible = 1;
keyboard_mode = 0;

// Set your app's input flag
my_input_active = 1;
```

### Detecting Keyboard Close

In your main loop or input handler:

```c
if (keyboard_visible) {
    keyboard_handle_touch(touch_x, touch_y);
    if (my_input_active && !keyboard_visible) {
        // Keyboard was closed (user pressed Enter)
        // keyboard_buf now contains the user's text
        strncpy(my_text, keyboard_buf, sizeof(my_text) - 1);
        my_input_active = 0;
        dirty = 1;
    }
    continue;
}
```

### Keyboard Layouts

| Mode | Row 1 | Row 2 | Row 3 |
|------|-------|-------|-------|
| abc | qwertyuiop | asdfghjkl | zxcvbnm |
| ABC | QWERTYUIOP | ASDFGHJKL | ZXCVBNM |
| #123 | .,!?@#$%&* | -+=/\|~() | ;:'"<>^ |

Special keys: **Shift/Mode** (cycles layouts), **Bksp** (backspace), **Enter** (closes keyboard), **SPACE** (full-width bar).

---

## Notifications

### Creating Notifications

```c
notif_add("Title", "Message text", "Action");
```

- `title`: Displayed in bold (max 127 chars)
- `message`: Displayed below title (max 255 chars)
- `action_label`: Button text, or `NULL`/`""` for no button (max 63 chars)

### Notification API

```c
int notif_get_active(void);         // Returns count of active notifications
void notif_dismiss(int index);      // Dismiss a notification by index
void notif_clear_all(void);         // Dismiss all notifications
```

### Max Notifications

```c
#define MAX_NOTIFICATIONS 16
```

When the limit is reached, the oldest notification is automatically removed.

### User Interaction

The user opens the notification sidebar by tapping the top-right corner of the screen. Tapping a notification's action button dismisses it. Tapping outside the sidebar closes it.

---

## Data Persistence

### File Locations

```c
#define DATA_DIR        "/mnt/us/extensions/kindlejap/data"
#define SETTINGS_FILE   "/mnt/us/extensions/kindlejap/data/settings.cfg"
#define APPSTATE_FILE   "/mnt/us/extensions/kindlejap/data/appstate.cfg"
```

### Saving Your App's Data

Use the `DATA_DIR` path to store your app's configuration:

```c
#define MY_APP_CONFIG "/mnt/us/extensions/kindlejap/data/myapp.cfg"

void my_save_config(void) {
    FILE *f = fopen(MY_APP_CONFIG, "w");
    if (!f) return;
    fprintf(f, "theme=%d\n", my_theme);
    fprintf(f, "font_size=%d\n", my_font_size);
    fprintf(f, "username=%s\n", my_username);
    fclose(f);
}

void my_load_config(void) {
    FILE *f = fopen(MY_APP_CONFIG, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "theme") == 0) my_theme = atoi(val);
        else if (strcmp(key, "font_size") == 0) my_font_size = atoi(val);
        else if (strcmp(key, "username") == 0) strncpy(my_username, val, 63);
    }
    fclose(f);
}
```

### Auto-Save

KindleJap auto-saves settings every 30 seconds and on clean exit. You can call your save function at any time.

### Logging

```c
log_msg("Something happened");
```

Appends a timestamped message to `/mnt/us/kindlejap.log`. Useful for debugging.

---

## E-Ink Display

### Refresh Functions

```c
void refresh_screen(void);            // Full refresh (clearer, slower)
void refresh_screen_partial(void);    // Partial refresh (faster, some ghosting)
```

### When to Use Each

| Function | When |
|----------|------|
| `refresh_screen()` | Full screen redraws, page transitions |
| `refresh_screen_partial()` | Small updates, scrolling, button presses |

The main loop calls `refresh_screen()` automatically after each frame when `dirty = 1`.

### The Dirty Flag

```c
static int dirty = 1;  // Set to 1 to request a screen redraw
```

Always set `dirty = 1` after modifying any visual state. The main loop only redraws when this flag is set, which saves battery.

---

## Package Manager

### Creating a Distributable App

To make your app installable via the Package Manager:

#### 1. Create `kindlejap-app.json`

Place this file at the root of your GitHub repository:

```json
{
    "name": "My App",
    "binary": "my-app",
    "version": "1.0.0",
    "description": "A useful Kindle app"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | App display name (max 63 chars) |
| `binary` | No | Binary asset name (defaults to `name`) |
| `version` | No | Version string displayed after install |
| `description` | No | Short description (max 127 chars) |

#### 2. Create a GitHub Release

Upload your compiled ARM binary as a release asset. The binary name should match the `binary` field (or `name` if not specified) with `-bin` appended.

Example for an app named "My App":
- Binary asset: `my-app-bin` (or `my-app-v1.0.0` — first download URL is used)

#### 3. Install Flow

When a user installs your app:
1. KindleJap fetches `kindlejap-app.json` from your repo
2. Downloads the binary from your latest GitHub Release
3. Saves it to `/mnt/us/extensions/kindlejap/apps/<name>/<binary>-bin`
4. Marks it as executable

### Installation Path

```
/mnt/us/extensions/kindlejap/apps/
    MyApp/
        MyApp-bin
```

### Uninstallation

Apps installed via Package Manager can be removed from the Package Manager UI. Built-in apps (Calculator, Files, Network, Browser, Package Manager) cannot be removed.

---

## Building & Compiling

### Requirements

- ARM cross-compiler: `arm-linux-gnueabi-gcc`
- GitHub Actions (CI/CD) or manual cross-compilation

### Build Command

```bash
arm-linux-gnueabi-gcc -O2 -Wall -Wextra -static \
    -o bin/kindlejap-bin \
    src/kindlejap.c \
    -lpthread
arm-linux-gnueabi-strip bin/kindlejap-bin
```

### Key Flags

| Flag | Purpose |
|------|---------|
| `-O2` | Optimization level 2 |
| `-static` | Static linking (no shared libs needed on Kindle) |
| `-lpthread` | POSIX threads |
| `-Wall -Wextra` | All warnings |

### Single-File Architecture

KindleJap is a single-file application (`src/kindlejap.c`). All code — drawing, apps, keyboard, browser, package manager — lives in one file. This makes it easy to compile and deploy.

### Deployment

After compiling, copy the binary to the Kindle:

```bash
cp kindlejap-bin /path/to/kindle/extensions/kindlejap/bin/kindlejap-bin
```

The binary is run by `bin/kindlejap.sh`, which is the entry point used by KUAL.

---

## Complete Example App

Here's a full example of a notes app with text input, persistence, and a clean UI:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTES_FILE "/mnt/us/extensions/kindlejap/data/notes.txt"

static char notes[4096] = "";
static int notes_len = 0;
static int input_active = 0;

static void notes_init(void) {
    FILE *f = fopen(NOTES_FILE, "r");
    if (f) {
        notes_len = fread(notes, 1, sizeof(notes) - 1, f);
        notes[notes_len] = 0;
        fclose(f);
    }
}

static void notes_save(void) {
    FILE *f = fopen(NOTES_FILE, "w");
    if (f) {
        fwrite(notes, 1, notes_len, f);
        fclose(f);
    }
}

static void notes_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);

    // Title
    draw_text(x + 10, y + 10, "My Notes", COLOR_BLACK, 3);

    // Note content area
    draw_rounded_rect(x + 10, y + 60, w - 20, h - 140, 8, COLOR_LIGHT);
    if (notes_len > 0) {
        // Render notes with word wrap
        int cx = x + 20, cy = y + 70;
        int max_w = w - 40;
        const char *p = notes;
        while (*p && cy < y + h - 100) {
            if (*p == '\n') {
                cx = x + 20;
                cy += 18;
                p++;
                continue;
            }
            draw_char(cx, cy, *p, COLOR_BLACK, 1);
            cx += FONT_W;
            if (cx > x + 20 + max_w) {
                cx = x + 20;
                cy += 18;
            }
            p++;
        }
    } else {
        draw_text_centered_in(x, y + h/2 - 10, w, "No notes yet", COLOR_MID, 2);
    }

    // Edit button
    draw_rounded_rect(x + 10, y + h - 60, w - 20, 44, 8, COLOR_DARK);
    draw_text_centered_in(x + 10, y + h - 50, w - 20, "Edit Notes", COLOR_WHITE, 2);
}

static void notes_handle(int tx, int ty, int released) {
    if (!released) return;

    // Edit button
    if (point_in_rect(tx, ty, 10, screen_height - TOPBAR_H - 60,
                      screen_width - 20, 44)) {
        strcpy(keyboard_buf, notes);
        keyboard_cursor = strlen(keyboard_buf);
        keyboard_visible = 1;
        input_active = 1;
    }
}

static App notes_app = {"Notes", notes_init, notes_draw, notes_handle, NULL};
```

---

## Best Practices

### Performance

- **Set `dirty = 1` only when needed.** The main loop only redraws when dirty, saving battery.
- **Use `refresh_screen_partial()` for small updates.** Full refreshes cause visible flashes.
- **Keep draw functions fast.** The main loop runs at 20 Hz (50ms per frame).

### UI Design

- **Use the full screen width (1072px).** Leave 10-20px padding on each side.
- **Scale 2 (16px) is ideal for body text.** Scale 1 is too small; scale 3+ is for titles only.
- **Use `COLOR_LIGHT` for card backgrounds** and `COLOR_WHITE` for the main background.
- **Use `draw_rounded_rect()` for buttons.** It looks much better than sharp rectangles.
- **Always provide visual feedback** when the user taps something.

### Input Handling

- **Always check `released` first.** Only process finger-lift events.
- **Use `point_in_rect()` for all hit testing.** It's the standard pattern.
- **Set `dirty = 1` after any state change.** Otherwise the screen won't update.
- **Keep keyboard input simple.** Copy text to `keyboard_buf`, set `keyboard_visible = 1`, detect close by checking `!keyboard_visible`.

### Data Storage

- **Use `DATA_DIR` (`/mnt/us/extensions/kindlejap/data/`) for your files.**
- **Use key=value format** for config files. It's simple and human-readable.
- **Save on important changes,** not just on exit. The device may lose power unexpectedly.

### E-Ink Specific

- **Avoid rapid full-screen redraws.** They cause visible flashing.
- **Use dark backgrounds sparingly.** They cause full-page refreshes on e-ink.
- **Test on real hardware.** E-ink ghosting behaves differently than LCD.
- **White-on-black text looks bold** on e-ink. Use it for emphasis.

### Memory

- **Static allocation preferred.** The Kindle has limited RAM.
- **Free temporary allocations immediately.** Don't let memory leak.
- **Use fixed-size buffers.** Avoid dynamic allocation in draw functions.

---

## Quick Reference

### Key Globals

| Variable | Type | Purpose |
|----------|------|---------|
| `screen_width` | `int` | Screen width in pixels (1072) |
| `screen_height` | `int` | Screen height in pixels (1448) |
| `dirty` | `int` | Set to 1 to request redraw |
| `keyboard_visible` | `int` | 1 when keyboard is showing |
| `keyboard_buf` | `char[128]` | Keyboard input buffer |
| `active_app_idx` | `int` | Index of currently active app |

### Key Functions

| Function | Purpose |
|----------|---------|
| `draw_rect(x,y,w,h,color)` | Fill rectangle |
| `draw_text(x,y,text,color,scale)` | Draw text |
| `draw_text_centered_in(x,y,w,text,color,scale)` | Centered text |
| `draw_rounded_rect(x,y,w,h,r,color)` | Rounded rectangle |
| `point_in_rect(px,py,x,y,w,h)` | Hit test |
| `text_width(text,scale)` | Get text pixel width |
| `notif_add(title,msg,action)` | Create notification |
| `log_msg(msg)` | Write to log file |
| `refresh_screen()` | Full e-ink refresh |
| `refresh_screen_partial()` | Fast partial refresh |

### Key Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `TOPBAR_H` | 40 | Status bar height |
| `KEYBOARD_H` | 360 | Keyboard height |
| `FONT_W` | 8 | Character width |
| `FONT_H` | 13 | Character height |
| `COLOR_WHITE` | 0xFF | White |
| `COLOR_BLACK` | 0x00 | Black |
| `COLOR_DARK` | 0x30 | Dark gray |
| `COLOR_MID` | 0xA0 | Medium gray |
| `COLOR_LIGHT` | 0xE8 | Light gray |
