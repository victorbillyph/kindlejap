#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>

// Framebuffer structures
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
int fb_fd = -1;
unsigned char *fb_mem = NULL;
int screen_width, screen_height, bytes_per_pixel;

// Input device
int input_fd = -1;

// Application state
volatile int running = 1;
volatile int menu_visible = 0;
volatile int menu_expanded = 0;

// Menu items
typedef struct {
    const char *name;
    int x, y, w, h;
    int active;
    void (*action)(void);
} MenuItem;

MenuItem menu_items[4];
int menu_item_count = 0;

// Colors (grayscale for e-ink)
#define COLOR_BLACK   0x00
#define COLOR_WHITE   0xFF
#define COLOR_GRAY    0x80
#define COLOR_LIGHT   0xC0
#define COLOR_DARK    0x40

// Taskbar dimensions
#define TASKBAR_HEIGHT 60
#define MENU_BUTTON_WIDTH 80

// Forward declarations
void init_framebuffer(void);
void init_input(void);
void cleanup(void);
void draw_rect(int x, int y, int w, int h, unsigned char color);
void draw_text(int x, int y, const char *text, unsigned char color, int scale);
void draw_taskbar(void);
void draw_menu(void);
void handle_touch(int x, int y, int pressed);
void handle_swipe(int start_x, int start_y, int end_x, int end_y);
void action_exit(void);
void action_apps(void);
void action_settings(void);
void signal_handler(int sig);

// Simple font data (5x7 pixels for each character)
static const unsigned char font5x7[][7] = {
    // Space
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // !
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    // A
    {0x08, 0x14, 0x22, 0x22, 0x3E, 0x22, 0x22},
    // B
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C},
    // C
    {0x1C, 0x22, 0x20, 0x20, 0x20, 0x22, 0x1C},
    // D
    {0x18, 0x24, 0x22, 0x22, 0x22, 0x24, 0x18},
    // E
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x3E},
    // F
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x20},
    // G
    {0x1C, 0x22, 0x20, 0x2E, 0x22, 0x22, 0x1C},
    // H
    {0x22, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x22},
    // I
    {0x1C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    // J
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x22, 0x1C},
    // K
    {0x22, 0x24, 0x28, 0x30, 0x28, 0x24, 0x22},
    // L
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3E},
    // M
    {0x22, 0x36, 0x2A, 0x2A, 0x22, 0x22, 0x22},
    // N
    {0x22, 0x32, 0x2A, 0x26, 0x22, 0x22, 0x22},
    // O
    {0x1C, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    // P
    {0x1C, 0x22, 0x22, 0x1C, 0x20, 0x20, 0x20},
    // Q
    {0x1C, 0x22, 0x22, 0x22, 0x2A, 0x24, 0x1A},
    // R
    {0x1C, 0x22, 0x22, 0x1C, 0x28, 0x24, 0x22},
    // S
    {0x1C, 0x22, 0x20, 0x1C, 0x02, 0x22, 0x1C},
    // T
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
    // U
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    // V
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x14, 0x08},
    // W
    {0x22, 0x22, 0x22, 0x2A, 0x2A, 0x36, 0x22},
    // X
    {0x22, 0x22, 0x14, 0x08, 0x14, 0x22, 0x22},
    // Y
    {0x22, 0x22, 0x14, 0x08, 0x08, 0x08, 0x08},
    // Z
    {0x3E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x3E},
    // a-z (lowercase mapped to uppercase for simplicity)
    {0x08, 0x14, 0x22, 0x22, 0x3E, 0x22, 0x22}, // a
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C}, // b
    {0x1C, 0x22, 0x20, 0x20, 0x20, 0x22, 0x1C}, // c
    {0x18, 0x24, 0x22, 0x22, 0x22, 0x24, 0x18}, // d
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x3E}, // e
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x20}, // f
    {0x1C, 0x22, 0x20, 0x2E, 0x22, 0x22, 0x1C}, // g
    {0x22, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x22}, // h
    {0x1C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C}, // i
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x22, 0x1C}, // j
    {0x22, 0x24, 0x28, 0x30, 0x28, 0x24, 0x22}, // k
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3E}, // l
    {0x22, 0x36, 0x2A, 0x2A, 0x22, 0x22, 0x22}, // m
    {0x22, 0x32, 0x2A, 0x26, 0x22, 0x22, 0x22}, // n
    {0x1C, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C}, // o
    {0x1C, 0x22, 0x22, 0x1C, 0x20, 0x20, 0x20}, // p
    {0x1C, 0x22, 0x22, 0x22, 0x2A, 0x24, 0x1A}, // q
    {0x1C, 0x22, 0x22, 0x1C, 0x28, 0x24, 0x22}, // r
    {0x1C, 0x22, 0x20, 0x1C, 0x02, 0x22, 0x1C}, // s
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08}, // t
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C}, // u
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x14, 0x08}, // v
    {0x22, 0x22, 0x22, 0x2A, 0x2A, 0x36, 0x22}, // w
    {0x22, 0x22, 0x14, 0x08, 0x14, 0x22, 0x22}, // x
    {0x22, 0x22, 0x14, 0x08, 0x08, 0x08, 0x08}, // y
    {0x3E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x3E}, // z
    // 0-9
    {0x1C, 0x22, 0x26, 0x2A, 0x32, 0x22, 0x1C}, // 0
    {0x08, 0x18, 0x08, 0x08, 0x08, 0x08, 0x1C}, // 1
    {0x1C, 0x22, 0x02, 0x0C, 0x10, 0x20, 0x3E}, // 2
    {0x3E, 0x02, 0x04, 0x0C, 0x02, 0x22, 0x1C}, // 3
    {0x04, 0x0C, 0x14, 0x24, 0x3E, 0x04, 0x04}, // 4
    {0x3E, 0x20, 0x3C, 0x02, 0x02, 0x22, 0x1C}, // 5
    {0x0C, 0x10, 0x20, 0x3C, 0x22, 0x22, 0x1C}, // 6
    {0x3E, 0x02, 0x04, 0x08, 0x10, 0x10, 0x10}, // 7
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C}, // 8
    {0x1C, 0x22, 0x22, 0x1E, 0x02, 0x04, 0x18}, // 9
};

void init_framebuffer(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("Failed to open framebuffer");
        exit(1);
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("Failed to get screen info");
        close(fb_fd);
        exit(1);
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("Failed to get fixed screen info");
        close(fb_fd);
        exit(1);
    }

    screen_width = vinfo.xres;
    screen_height = vinfo.yres;
    bytes_per_pixel = vinfo.bits_per_pixel / 8;

    size_t mem_size = finfo.smem_len;
    fb_mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("Failed to mmap framebuffer");
        close(fb_fd);
        exit(1);
    }

    printf("Screen: %dx%d, %d bpp\n", screen_width, screen_height, vinfo.bits_per_pixel);
}

void init_input(void) {
    // Try to open touch input device
    const char *input_devices[] = {
        "/dev/input/event1",
        "/dev/input/event0",
        NULL
    };

    for (int i = 0; input_devices[i] != NULL; i++) {
        input_fd = open(input_devices[i], O_RDONLY | O_NONBLOCK);
        if (input_fd >= 0) {
            printf("Opened input device: %s\n", input_devices[i]);
            break;
        }
    }

    if (input_fd < 0) {
        fprintf(stderr, "Warning: No input device found\n");
    }
}

void cleanup(void) {
    if (fb_mem != NULL) {
        munmap(fb_mem, finfo.smem_len);
    }
    if (fb_fd >= 0) {
        close(fb_fd);
    }
    if (input_fd >= 0) {
        close(input_fd);
    }
}

void draw_rect(int x, int y, int w, int h, unsigned char color) {
    for (int j = y; j < y + h && j < screen_height; j++) {
        for (int i = x; i < x + w && i < screen_width; i++) {
            int offset = (j * finfo.line_length) + (i * bytes_per_pixel);
            if (offset >= 0 && offset < (int)finfo.smem_len) {
                fb_mem[offset] = color;
                if (bytes_per_pixel > 1) {
                    fb_mem[offset + 1] = color;
                }
            }
        }
    }
}

void draw_char(int x, int y, char c, unsigned char color, int scale) {
    int char_index = 0;

    if (c == ' ') {
        char_index = 0;
    } else if (c == '!') {
        char_index = 1;
    } else if (c >= 'A' && c <= 'Z') {
        char_index = 2 + (c - 'A');
    } else if (c >= 'a' && c <= 'z') {
        char_index = 2 + (c - 'a');
    } else if (c >= '0' && c <= '9') {
        char_index = 28 + (c - '0');
    } else {
        return;
    }

    for (int row = 0; row < 7; row++) {
        unsigned char bits = font5x7[char_index][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x20 >> col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + (col * scale) + sx;
                        int py = y + (row * scale) + sy;
                        if (px < screen_width && py < screen_height) {
                            int offset = (py * finfo.line_length) + (px * bytes_per_pixel);
                            if (offset >= 0 && offset < (int)finfo.smem_len) {
                                fb_mem[offset] = color;
                                if (bytes_per_pixel > 1) {
                                    fb_mem[offset + 1] = color;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void draw_text(int x, int y, const char *text, unsigned char color, int scale) {
    int curr_x = x;
    while (*text) {
        draw_char(curr_x, y, *text, color, scale);
        curr_x += 6 * scale;
        text++;
    }
}

void draw_taskbar(void) {
    int taskbar_y = screen_height - TASKBAR_HEIGHT;

    // Draw taskbar background
    draw_rect(0, taskbar_y, screen_width, TASKBAR_HEIGHT, COLOR_LIGHT);

    // Draw separator line
    draw_rect(0, taskbar_y, screen_width, 2, COLOR_DARK);

    // Draw menu button
    int btn_x = 10;
    int btn_y = taskbar_y + 10;
    int btn_w = MENU_BUTTON_WIDTH;
    int btn_h = TASKBAR_HEIGHT - 20;

    draw_rect(btn_x, btn_y, btn_w, btn_h, COLOR_GRAY);

    // Draw "MENU" text centered in button
    int text_w = 5 * 5; // 5 chars * 5 pixels wide
    int text_x = btn_x + (btn_w - text_w) / 2;
    int text_y = btn_y + (btn_h - 7) / 2;
    draw_text(text_x, text_y, "MENU", COLOR_BLACK, 1);

    // Draw "KindleJap" title
    draw_text(screen_width - 200, taskbar_y + 20, "KindleJap", COLOR_BLACK, 2);
}

void draw_menu(void) {
    if (!menu_expanded) return;

    int menu_width = 250;
    int menu_height = 200;
    int menu_x = 10;
    int menu_y = screen_height - TASKBAR_HEIGHT - menu_height - 10;

    // Draw menu background
    draw_rect(menu_x, menu_y, menu_width, menu_height, COLOR_WHITE);

    // Draw menu border
    draw_rect(menu_x, menu_y, menu_width, 2, COLOR_BLACK);
    draw_rect(menu_x, menu_y + menu_height - 2, menu_width, 2, COLOR_BLACK);
    draw_rect(menu_x, menu_y, 2, menu_height, COLOR_BLACK);
    draw_rect(menu_x + menu_width - 2, menu_y, 2, menu_height, COLOR_BLACK);

    // Draw menu items
    int item_height = 40;
    int item_y = menu_y + 10;

    // Apps
    draw_rect(menu_x + 10, item_y, menu_width - 20, item_height, COLOR_LIGHT);
    draw_text(menu_x + 20, item_y + 15, "Apps", COLOR_BLACK, 2);
    menu_items[0].x = menu_x + 10;
    menu_items[0].y = item_y;
    menu_items[0].w = menu_width - 20;
    menu_items[0].h = item_height;
    menu_items[0].active = 1;
    item_y += item_height + 5;

    // Settings
    draw_rect(menu_x + 10, item_y, menu_width - 20, item_height, COLOR_LIGHT);
    draw_text(menu_x + 20, item_y + 15, "Settings", COLOR_BLACK, 2);
    menu_items[1].x = menu_x + 10;
    menu_items[1].y = item_y;
    menu_items[1].w = menu_width - 20;
    menu_items[1].h = item_height;
    menu_items[1].active = 1;
    item_y += item_height + 5;

    // Exit
    draw_rect(menu_x + 10, item_y, menu_width - 20, item_height, COLOR_LIGHT);
    draw_text(menu_x + 20, item_y + 15, "Exit", COLOR_BLACK, 2);
    menu_items[2].x = menu_x + 10;
    menu_items[2].y = item_y;
    menu_items[2].w = menu_width - 20;
    menu_items[2].h = item_height;
    menu_items[2].active = 1;
}

void action_exit(void) {
    printf("Exiting KindleJap...\n");
    running = 0;
}

void action_apps(void) {
    printf("Opening Apps menu...\n");
    // TODO: Implement apps menu
}

void action_settings(void) {
    printf("Opening Settings...\n");
    // TODO: Implement settings
}

void redraw_screen(void) {
    // Clear screen
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);

    // Draw taskbar
    draw_taskbar();

    // Draw menu if expanded
    draw_menu();
}

void handle_touch(int x, int y, int pressed) {
    static int touch_start_x = 0;
    static int touch_start_y = 0;
    static int swipe_detected = 0;

    if (pressed) {
        touch_start_x = x;
        touch_start_y = y;
        swipe_detected = 0;

        // Check if touching the bottom area (taskbar region)
        if (y > screen_height - TASKBAR_HEIGHT - 50) {
            // Potential swipe down
            swipe_detected = 1;
        }
    } else {
        // Touch released
        int dx = x - touch_start_x;
        int dy = y - touch_start_y;

        // Detect swipe down
        if (swipe_detected && dy > 50 && abs(dx) < 30) {
            menu_expanded = !menu_expanded;
            redraw_screen();
            return;
        }

        // Detect tap on menu button
        if (x >= 10 && x <= 10 + MENU_BUTTON_WIDTH &&
            y >= screen_height - TASKBAR_HEIGHT + 10 &&
            y <= screen_height - 10) {
            menu_expanded = !menu_expanded;
            redraw_screen();
            return;
        }

        // Detect tap on menu items
        if (menu_expanded) {
            for (int i = 0; i < 3; i++) {
                if (menu_items[i].active &&
                    x >= menu_items[i].x && x <= menu_items[i].x + menu_items[i].w &&
                    y >= menu_items[i].y && y <= menu_items[i].y + menu_items[i].h) {
                    switch (i) {
                        case 0:
                            action_apps();
                            break;
                        case 1:
                            action_settings();
                            break;
                        case 2:
                            action_exit();
                            break;
                    }
                    menu_expanded = 0;
                    redraw_screen();
                    return;
                }
            }
        }
    }
}

void process_input(void) {
    struct input_event ev;

    while (read(input_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == EV_ABS) {
            static int last_x = 0, last_y = 0;

            if (ev.code == ABS_MT_POSITION_X) {
                last_x = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                last_y = ev.value;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            handle_touch(0, 0, ev.value);
        }
    }
}

void signal_handler(int sig) {
    running = 0;
}

int main(int argc, char *argv[]) {
    printf("KindleJap Launcher starting...\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize systems
    init_framebuffer();
    init_input();

    // Initialize menu items
    menu_items[0].action = action_apps;
    menu_items[1].action = action_settings;
    menu_items[2].action = action_exit;

    // Initial draw
    redraw_screen();

    printf("KindleJap Launcher running. Touch the bottom of screen or tap MENU button.\n");

    // Main loop
    while (running) {
        if (input_fd >= 0) {
            process_input();
        }
        usleep(50000); // 50ms refresh
    }

    // Cleanup
    cleanup();

    printf("KindleJap Launcher exited.\n");
    return 0;
}
