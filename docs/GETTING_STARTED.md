# Getting Started with KindleJap Development

Step-by-step guide to build your first KindleJap app.

## Prerequisites

- Linux with `arm-linux-gnueabi-gcc` cross-compiler
- A Kindle Paperwhite (or compatible device) connected via USB
- GitHub account (for distributing your app)

### Install Cross-Compiler

```bash
# Debian/Ubuntu
sudo apt install gcc-arm-linux-gnueabi

# Verify
arm-linux-gnueabi-gcc --version
```

## Step 1: Create Your App File

Create `myapp.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

// --- KindleJap SDK (copy from kindlejap.c or link) ---

#define COLOR_WHITE  0xFF
#define COLOR_BLACK  0x00
#define COLOR_DARK   0x30
#define COLOR_MID    0xA0
#define COLOR_LIGHT  0xE8
#define FONT_W 8
#define FONT_H 13

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
unsigned char *fb_mem = NULL;
int screen_width, screen_height;
int dirty = 1;

void draw_pixel(int x, int y, unsigned char c) {
    if (x<0||x>=screen_width||y<0||y>=screen_height) return;
    int off = y*finfo.line_length+x;
    if (off>=0 && off<(int)finfo.smem_len)
        fb_mem[off] = c;
}

void draw_rect(int x, int y, int w, int h, unsigned char c) {
    for (int j=y; j<y+h; j++)
        for (int i=x; i<x+w; i++)
            draw_pixel(i, j, c);
}

// Add draw_text, draw_char, draw_rounded_rect etc.
// (copy from kindlejap.c or implement your own)

int point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px>=x && px<x+w && py>=y && py<y+h;
}

// --- Your App ---

static int counter = 0;

void myapp_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);

    // Title
    // draw_text_centered_in(x, y+40, w, "Hello Kindle!", COLOR_BLACK, 4);

    // Counter
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", counter);
    // draw_text_centered_in(x, y+150, w, buf, COLOR_BLACK, 5);

    // Button
    draw_rounded_rect(x+w/2-100, y+280, 200, 60, 10, COLOR_DARK);
    // draw_text_centered_in(x+w/2-100, y+290, 200, "TAP ME", COLOR_WHITE, 3);
}

void myapp_handle(int tx, int ty, int released) {
    if (!released) return;
    if (point_in_rect(tx, ty, screen_width/2-100, 40+280, 200, 60)) {
        counter++;
        dirty = 1;
    }
}

int main(void) {
    // Open framebuffer
    int fd = open("/dev/fb0", O_RDWR);
    ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fd, FBIOGET_FSCREENINFO, &finfo);
    screen_width = vinfo.xres;
    screen_height = vinfo.yres;
    fb_mem = mmap(0, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    // Main loop
    while (1) {
        if (dirty) {
            myapp_draw(0, 40, screen_width, screen_height-40);
            dirty = 0;
        }
        usleep(50000);
    }

    return 0;
}
```

## Step 2: Compile

```bash
arm-linux-gnueabi-gcc -O2 -static -o myapp-bin myapp.c
arm-linux-gnueabi-strip myapp-bin
chmod +x myapp-bin
```

## Step 3: Deploy to Kindle

Connect your Kindle via USB and copy:

```bash
cp myapp-bin /media/kindle/extensions/kindlejap/apps/MyApp/myapp-bin
```

## Step 4: Integrate with KindleJap

To make your app appear in the KindleJap menu, add it to `kindlejap.c`:

```c
// Forward declare your handlers
extern void myapp_draw(int x, int y, int w, int h);
extern void myapp_handle(int tx, int ty, int released);

// Create App struct
static App myapp_app = {"MyApp", NULL, myapp_draw, myapp_handle, NULL};

// In main(), before the event loop:
app_register(&myapp_app);
```

Then recompile KindleJap itself.

## Step 5: Distribute via Package Manager

1. Create a GitHub repo for your app
2. Add `kindlejap-app.json` to the root:

```json
{
    "name": "MyApp",
    "binary": "myapp",
    "version": "1.0.0",
    "description": "My awesome Kindle app"
}
```

3. Create a GitHub Release with your binary as an asset
4. Users install from Package Manager using: `username/repo`

## Next Steps

- Read the [Developer Guide](DEVELOPER_GUIDE.md) for complete API docs
- Check the [Quick Reference](QUICK_REFERENCE.md) for function signatures
- Study built-in apps in `src/kindlejap.c` for patterns
