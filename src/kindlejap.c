#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>

// Version
#define KINDLEJAP_VERSION "1.1.1"

// GitHub API URL for releases
#define GITHUB_API_URL "https://api.github.com/repos/victorbillyph/kindlejap/releases/latest"
#define UPDATE_DOWNLOAD_URL "https://github.com/victorbillyph/kindlejap/archive/refs/heads/master.zip"
#define UPDATE_SCRIPT "/mnt/us/extensions/japlat/bin/update.sh"

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

// Touch state
int touch_last_x = 0;
int touch_last_y = 0;

// Update state
volatile int show_update_dialog = 0;
volatile int update_available = 0;
volatile int update_downloading = 0;
volatile int update_progress = 0;
char latest_version[32] = "";
char update_notes[1024] = "";

// Menu items
typedef struct {
    const char *name;
    int x, y, w, h;
    int active;
    void (*action)(void);
} MenuItem;

MenuItem menu_items[5];
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
void draw_update_dialog(void);
void handle_touch(int x, int y, int pressed);
void handle_swipe(int start_x, int start_y, int end_x, int end_y);
void action_exit(void);
void action_apps(void);
void action_settings(void);
void action_check_update(void);
void action_update_now(void);
void action_update_yes(void);
void action_update_no(void);
void redraw_screen(void);
void signal_handler(int sig);
int check_for_updates(void);
int compare_versions(const char *v1, const char *v2);
int download_and_update(void);

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

int compare_versions(const char *v1, const char *v2) {
    int major1, minor1, patch1;
    int major2, minor2, patch2;

    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

int check_for_updates(void) {
    printf("Checking for updates...\n");

    // Use curl to fetch GitHub API
    const char *cmd = "curl -s " GITHUB_API_URL " 2>/dev/null";
    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to check for updates\n");
        return -1;
    }

    char buffer[4096];
    char tag_name[32] = "";
    char body[1024] = "";
    int in_body = 0;
    int body_idx = 0;

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // Parse tag_name
        char *tag = strstr(buffer, "\"tag_name\":");
        if (tag) {
            char *start = strchr(tag + 11, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len < 32) {
                        strncpy(tag_name, start, len);
                        tag_name[len] = '\0';
                    }
                }
            }
        }

        // Parse body (simplified - just get first 200 chars)
        char *body_start = strstr(buffer, "\"body\":");
        if (body_start && !in_body) {
            in_body = 1;
            char *start = strchr(body_start + 7, '"');
            if (start) {
                start++;
                int i = 0;
                while (*start && *start != '"' && i < 200) {
                    if (*start == '\\' && *(start+1) == 'n') {
                        body[body_idx++] = '\n';
                        start += 2;
                    } else {
                        body[body_idx++] = *start;
                        start++;
                    }
                    i++;
                }
                body[body_idx] = '\0';
                in_body = 0;
            }
        }
    }

    pclose(fp);

    // If we got a tag name, compare versions
    if (strlen(tag_name) > 0) {
        // Remove 'v' prefix if present
        char *ver = tag_name;
        if (ver[0] == 'v') ver++;

        strcpy(latest_version, ver);
        strncpy(update_notes, body, sizeof(update_notes) - 1);

        printf("Current: %s, Latest: %s\n", KINDLEJAP_VERSION, latest_version);

        if (compare_versions(KINDLEJAP_VERSION, latest_version) < 0) {
            printf("Update available!\n");
            return 1; // Update available
        } else {
            printf("Already up to date.\n");
            return 0; // Up to date
        }
    }

    return -1; // Error parsing
}

void *download_and_update_thread(void *arg) {
    (void)arg;
    printf("Starting update...\n");
    update_downloading = 1;
    update_progress = 0;
    redraw_screen();

    // Run the update script
    int ret = system(UPDATE_SCRIPT " " KINDLEJAP_VERSION " " UPDATE_DOWNLOAD_URL);

    update_downloading = 0;

    if (ret == 0) {
        printf("Update completed successfully!\n");
        update_progress = 100;
        redraw_screen();
        sleep(2);
        // Restart the app
        execl(UPDATE_SCRIPT, UPDATE_SCRIPT, "restart", NULL);
        return NULL;
    } else {
        printf("Update failed!\n");
        update_progress = -1;
        redraw_screen();
        sleep(2);
        return NULL;
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

void draw_update_dialog(void) {
    if (!show_update_dialog) return;

    // Dialog box
    int dlg_width = 400;
    int dlg_height = 250;
    int dlg_x = (screen_width - dlg_width) / 2;
    int dlg_y = (screen_height - dlg_height) / 2;

    // Background overlay
    draw_rect(0, 0, screen_width, screen_height, COLOR_DARK);

    // Dialog background
    draw_rect(dlg_x, dlg_y, dlg_width, dlg_height, COLOR_WHITE);

    // Border
    draw_rect(dlg_x, dlg_y, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y + dlg_height - 2, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y, 2, dlg_height, COLOR_BLACK);
    draw_rect(dlg_x + dlg_width - 2, dlg_y, 2, dlg_height, COLOR_BLACK);

    // Title
    draw_text(dlg_x + 20, dlg_y + 20, "UPDATE AVAILABLE", COLOR_BLACK, 2);

    // Version info
    char version_text[64];
    snprintf(version_text, sizeof(version_text), "Current: %s", KINDLEJAP_VERSION);
    draw_text(dlg_x + 20, dlg_y + 50, version_text, COLOR_BLACK, 1);

    snprintf(version_text, sizeof(version_text), "Latest:  %s", latest_version);
    draw_text(dlg_x + 20, dlg_y + 65, version_text, COLOR_BLACK, 1);

    // Update notes (simplified - show first line)
    draw_text(dlg_x + 20, dlg_y + 90, "New version available!", COLOR_BLACK, 1);

    // Yes button
    int btn_w = 120;
    int btn_h = 40;
    int btn_y = dlg_y + dlg_height - 60;

    draw_rect(dlg_x + 60, btn_y, btn_w, btn_h, COLOR_GRAY);
    draw_text(dlg_x + 80, btn_y + 15, "UPDATE", COLOR_BLACK, 2);

    // No button
    draw_rect(dlg_x + 220, btn_y, btn_w, btn_h, COLOR_GRAY);
    draw_text(dlg_x + 250, btn_y + 15, "CANCEL", COLOR_BLACK, 2);

    // Store button coordinates for touch handling
    menu_items[3].x = dlg_x + 60;
    menu_items[3].y = btn_y;
    menu_items[3].w = btn_w;
    menu_items[3].h = btn_h;

    menu_items[4].x = dlg_x + 220;
    menu_items[4].y = btn_y;
    menu_items[4].w = btn_w;
    menu_items[4].h = btn_h;
}

void draw_update_progress(void) {
    if (!update_downloading) return;

    // Dialog box
    int dlg_width = 400;
    int dlg_height = 150;
    int dlg_x = (screen_width - dlg_width) / 2;
    int dlg_y = (screen_height - dlg_height) / 2;

    // Background overlay
    draw_rect(0, 0, screen_width, screen_height, COLOR_DARK);

    // Dialog background
    draw_rect(dlg_x, dlg_y, dlg_width, dlg_height, COLOR_WHITE);

    // Border
    draw_rect(dlg_x, dlg_y, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y + dlg_height - 2, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y, 2, dlg_height, COLOR_BLACK);
    draw_rect(dlg_x + dlg_width - 2, dlg_y, 2, dlg_height, COLOR_BLACK);

    // Title
    draw_text(dlg_x + 20, dlg_y + 20, "UPDATING...", COLOR_BLACK, 2);

    // Progress bar background
    int bar_x = dlg_x + 20;
    int bar_y = dlg_y + 70;
    int bar_w = dlg_width - 40;
    int bar_h = 20;

    draw_rect(bar_x, bar_y, bar_w, bar_h, COLOR_LIGHT);

    // Progress bar fill
    if (update_progress > 0) {
        int fill_w = (bar_w * update_progress) / 100;
        draw_rect(bar_x, bar_y, fill_w, bar_h, COLOR_DARK);
    }

    // Progress text
    char progress_text[32];
    snprintf(progress_text, sizeof(progress_text), "%d%%", update_progress);
    draw_text(dlg_x + 180, bar_y + 25, progress_text, COLOR_BLACK, 1);
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

void *check_updates_thread(void *arg) {
    (void)arg;
    int result = check_for_updates();
    if (result == 1) {
        show_update_dialog = 1;
        update_available = 1;
        redraw_screen();
    }
    return NULL;
}

void action_check_update(void) {
    printf("Checking for updates...\n");
    menu_expanded = 0;

    // Show checking message
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
    draw_taskbar();
    draw_text(100, screen_height / 2 - 20, "Checking for updates...", COLOR_BLACK, 2);
    redraw_screen();

    int result = check_for_updates();

    if (result == 1) {
        // Update available
        show_update_dialog = 1;
        update_available = 1;
    } else if (result == 0) {
        // Up to date
        draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
        draw_taskbar();
        draw_text(100, screen_height / 2 - 20, "Already up to date!", COLOR_BLACK, 2);
        redraw_screen();
        sleep(2);
    } else {
        // Error
        draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
        draw_taskbar();
        draw_text(100, screen_height / 2 - 20, "Check failed!", COLOR_BLACK, 2);
        draw_text(100, screen_height / 2 + 10, "No internet connection?", COLOR_BLACK, 1);
        redraw_screen();
        sleep(2);
    }

    update_available = (result == 1) ? 1 : 0;
    redraw_screen();
}

void action_update_now(void) {
    printf("Starting update...\n");
    menu_expanded = 0;
    show_update_dialog = 0;
    update_downloading = 1;
    update_progress = 0;
    redraw_screen();

    // Run update in background thread
    pthread_t update_thread;
    pthread_create(&update_thread, NULL, download_and_update_thread, NULL);
    pthread_detach(update_thread);
}

void action_update_yes(void) {
    action_update_now();
}

void action_update_no(void) {
    show_update_dialog = 0;
    update_available = 0;
    redraw_screen();
}

void redraw_screen(void) {
    // Clear screen
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);

    // Draw taskbar
    draw_taskbar();

    // Draw menu if expanded
    draw_menu();

    // Draw update dialog if visible
    draw_update_dialog();

    // Draw update progress if downloading
    draw_update_progress();
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
            for (int i = 0; i < 5; i++) {
                if (i < 3 || (i == 3 && show_update_dialog) || (i == 4 && show_update_dialog)) {
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
                            case 3:
                                action_update_yes();
                                break;
                            case 4:
                                action_update_no();
                                break;
                        }
                        menu_expanded = 0;
                        redraw_screen();
                        return;
                    }
                }
            }
        }

        // Detect tap on update dialog buttons
        if (show_update_dialog) {
            // Yes button
            if (x >= menu_items[3].x && x <= menu_items[3].x + menu_items[3].w &&
                y >= menu_items[3].y && y <= menu_items[3].y + menu_items[3].h) {
                action_update_yes();
                redraw_screen();
                return;
            }

            // No button
            if (x >= menu_items[4].x && x <= menu_items[4].x + menu_items[4].w &&
                y >= menu_items[4].y && y <= menu_items[4].y + menu_items[4].h) {
                action_update_no();
                redraw_screen();
                return;
            }
        }
    }
}

void process_input(void) {
    struct input_event ev;

    while (read(input_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) {
                touch_last_x = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                touch_last_y = ev.value;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            handle_touch(touch_last_x, touch_last_y, ev.value);
        }
    }
}

void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
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
    menu_items[3].action = action_update_yes;
    menu_items[4].action = action_update_no;

    // Initial draw
    redraw_screen();

    // Check for updates on startup (in background)
    pthread_t update_check_thread;
    pthread_create(&update_check_thread, NULL, check_updates_thread, NULL);
    pthread_detach(update_check_thread);

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
