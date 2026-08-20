# KindleJap API Quick Reference

## Drawing

```c
draw_pixel(x, y, color)
draw_rect(x, y, w, h, color)
draw_rounded_rect(x, y, w, h, radius, color)
draw_circle(cx, cy, r, color)
draw_line(x0, y0, x1, y1, color)
draw_char(x, y, ch, color, scale)
draw_text(x, y, text, color, scale)
draw_text_centered_in(x, y, w, text, color, scale)
draw_text_right(x_right, y, text, color, scale)
draw_bmp(path, x, y)
draw_pgm(path, x, y)
text_width(text, scale) -> int
```

## Colors

```c
COLOR_WHITE  0xFF
COLOR_BLACK  0x00
COLOR_DARK   0x30
COLOR_MID    0xA0
COLOR_LIGHT  0xE8
```

## Font

```c
FONT_W  8   // char width
FONT_H  13  // char height
// Scale: actual size = FONT_W*scale x FONT_H*scale
```

## Screen

```c
screen_width      // 1072 on Kindle Paperwhite
screen_height     // 1448 on Kindle Paperwhite
TOPBAR_H    40    // status bar height
KEYBOARD_H 360    // keyboard height
// App area: (0, TOPBAR_H) to (screen_width, screen_height)
```

## Touch

```c
point_in_rect(px, py, x, y, w, h) -> int  // 1=inside
// App handler: void on_touch(int tx, int ty, int released)
// released is always 1 when called
```

## Keyboard

```c
keyboard_visible     // int: 1=open
keyboard_mode        // int: 0=abc, 1=ABC, 2=#123
keyboard_cursor      // int: cursor position
keyboard_buf[128]    // char: input buffer
// Open: set keyboard_visible=1, copy text to keyboard_buf
// Close detected: check !keyboard_visible after keyboard_handle_touch()
```

## Notifications

```c
notif_add("Title", "Message", "Action Label")
notif_get_active() -> int
notif_dismiss(index)
notif_clear_all()
```

## Data Persistence

```c
DATA_DIR "/mnt/us/extensions/kindlejap/data"
// Write your own files there using FILE* I/O
// Key=value format recommended for config
```

## E-Ink

```c
refresh_screen()            // full refresh
refresh_screen_partial()    // fast partial
dirty = 1                   // request redraw
```

## Logging

```c
log_msg("message")  // writes to /mnt/us/kindlejap.log
```

## App Struct

```c
typedef struct {
    const char *name;
    void (*init)(void);
    void (*draw)(int x, int y, int w, int h);
    void (*on_touch)(int x, int y, int released);
    void (*cleanup)(void);
} App;
```

## Package Manager Manifest

```json
{
    "name": "App Name",
    "binary": "app-binary-name",
    "version": "1.0.0",
    "description": "Short description"
}
```

## Build

```bash
arm-linux-gnueabi-gcc -O2 -Wall -Wextra -static -o app-bin app.c -lpthread
arm-linux-gnueabi-strip app-bin
```
