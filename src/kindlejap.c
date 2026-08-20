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
#include <errno.h>
#include <time.h>

#define KINDLEJAP_VERSION "1.2.1"
#define GITHUB_API_URL "https://api.github.com/repos/victorbillyph/kindlejap/releases/latest"
#define UPDATE_SCRIPT "/mnt/us/extensions/kindlejap/bin/update.sh"
#define LOCKFILE "/tmp/kindlejap.lock"
#define LOGFILE "/mnt/us/kindlejap.log"

static FILE *logfp = NULL;
static int log_initialized = 0;

void log_msg(const char *msg) {
    if (!logfp) {
        logfp = fopen(LOGFILE, "w");
        log_initialized = 1;
    }
    if (logfp) {
        fprintf(logfp, "%s\n", msg);
        fflush(logfp);
    }
    printf("%s\n", msg);
}

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
int fb_fd = -1;
unsigned char *fb_mem = NULL;
int screen_width, screen_height, bytes_per_pixel;
int input_fd = -1;
volatile int running = 1;
volatile int menu_visible = 0;
volatile int menu_expanded = 0;
int touch_last_x = 0;
int touch_last_y = 0;
volatile int show_update_dialog = 0;
volatile int update_available = 0;
volatile int update_downloading = 0;
volatile int update_progress = 0;
char latest_version[32] = "";
char update_notes[1024] = "";
volatile int splash_done = 0;

typedef struct {
    const char *name;
    int x, y, w, h;
    int active;
    void (*action)(void);
} MenuItem;

MenuItem menu_items[5];
int menu_item_count = 0;

#define COLOR_BLACK   0x00
#define COLOR_WHITE   0xFF
#define COLOR_GRAY    0x80
#define COLOR_LIGHT   0xC0
#define COLOR_DARK    0x40

#define TASKBAR_HEIGHT 60
#define MENU_BUTTON_WIDTH 80

void init_framebuffer(void);
void init_input(void);
void cleanup(void);
void draw_rect(int x, int y, int w, int h, unsigned char color);
void draw_char(int x, int y, char c, unsigned char color, int scale);
void draw_text(int x, int y, const char *text, unsigned char color, int scale);
void draw_taskbar(void);
void draw_menu(void);
void draw_update_dialog(void);
void handle_touch(int x, int y, int pressed);
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
void show_splash(void);
void show_init_status(const char *msg);
int acquire_lock(void);
void release_lock(void);
void restore_kindle_ui(void);

static const unsigned char font5x7[][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    {0x08, 0x14, 0x22, 0x22, 0x3E, 0x22, 0x22},
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C},
    {0x1C, 0x22, 0x20, 0x20, 0x20, 0x22, 0x1C},
    {0x18, 0x24, 0x22, 0x22, 0x22, 0x24, 0x18},
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x3E},
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x20},
    {0x1C, 0x22, 0x20, 0x2E, 0x22, 0x22, 0x1C},
    {0x22, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x22},
    {0x1C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x22, 0x1C},
    {0x22, 0x24, 0x28, 0x30, 0x28, 0x24, 0x22},
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3E},
    {0x22, 0x36, 0x2A, 0x2A, 0x22, 0x22, 0x22},
    {0x22, 0x32, 0x2A, 0x26, 0x22, 0x22, 0x22},
    {0x1C, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    {0x1C, 0x22, 0x22, 0x1C, 0x20, 0x20, 0x20},
    {0x1C, 0x22, 0x22, 0x22, 0x2A, 0x24, 0x1A},
    {0x1C, 0x22, 0x22, 0x1C, 0x28, 0x24, 0x22},
    {0x1C, 0x22, 0x20, 0x1C, 0x02, 0x22, 0x1C},
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x14, 0x08},
    {0x22, 0x22, 0x22, 0x2A, 0x2A, 0x36, 0x22},
    {0x22, 0x22, 0x14, 0x08, 0x14, 0x22, 0x22},
    {0x22, 0x22, 0x14, 0x08, 0x08, 0x08, 0x08},
    {0x3E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x3E},
    {0x08, 0x14, 0x22, 0x22, 0x3E, 0x22, 0x22},
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C},
    {0x1C, 0x22, 0x20, 0x20, 0x20, 0x22, 0x1C},
    {0x18, 0x24, 0x22, 0x22, 0x22, 0x24, 0x18},
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x3E},
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x20},
    {0x1C, 0x22, 0x20, 0x2E, 0x22, 0x22, 0x1C},
    {0x22, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x22},
    {0x1C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x22, 0x1C},
    {0x22, 0x24, 0x28, 0x30, 0x28, 0x24, 0x22},
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3E},
    {0x22, 0x36, 0x2A, 0x2A, 0x22, 0x22, 0x22},
    {0x22, 0x32, 0x2A, 0x26, 0x22, 0x22, 0x22},
    {0x1C, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    {0x1C, 0x22, 0x22, 0x1C, 0x20, 0x20, 0x20},
    {0x1C, 0x22, 0x22, 0x22, 0x2A, 0x24, 0x1A},
    {0x1C, 0x22, 0x22, 0x1C, 0x28, 0x24, 0x22},
    {0x1C, 0x22, 0x20, 0x1C, 0x02, 0x22, 0x1C},
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x14, 0x08},
    {0x22, 0x22, 0x22, 0x2A, 0x2A, 0x36, 0x22},
    {0x22, 0x22, 0x14, 0x08, 0x14, 0x22, 0x22},
    {0x22, 0x22, 0x14, 0x08, 0x08, 0x08, 0x08},
    {0x3E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x3E},
    {0x1C, 0x22, 0x26, 0x2A, 0x32, 0x22, 0x1C},
    {0x08, 0x18, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x1C, 0x22, 0x02, 0x0C, 0x10, 0x20, 0x3E},
    {0x3E, 0x02, 0x04, 0x0C, 0x02, 0x22, 0x1C},
    {0x04, 0x0C, 0x14, 0x24, 0x3E, 0x04, 0x04},
    {0x3E, 0x20, 0x3C, 0x02, 0x02, 0x22, 0x1C},
    {0x0C, 0x10, 0x20, 0x3C, 0x22, 0x22, 0x1C},
    {0x3E, 0x02, 0x04, 0x08, 0x10, 0x10, 0x10},
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C},
    {0x1C, 0x22, 0x22, 0x1E, 0x02, 0x04, 0x18},
};

int acquire_lock(void) {
    log_msg("acquire_lock: opening " LOCKFILE);
    int fd = open(LOCKFILE, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        log_msg("acquire_lock: failed to open lockfile");
        return -1;
    }

    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLK, &fl) < 0) {
        log_msg("acquire_lock: lock held by another process");
        close(fd);
        return -1;
    }

    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    ftruncate(fd, 0);
    write(fd, pid_str, strlen(pid_str));

    log_msg("acquire_lock: lock acquired");
    return fd;
}

void release_lock(void) {
    unlink(LOCKFILE);
}

void restore_kindle_ui(void) {
    printf("Restoring Kindle UI...\n");
    system("initctl start lab126_gui 2>/dev/null");
    system("initctl start otaupd 2>/dev/null");
    system("initctl start phd 2>/dev/null");
    system("initctl start tmd 2>/dev/null");
    system("initctl start todo 2>/dev/null");
    system("initctl start mcsd 2>/dev/null");
    system("lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null");
}

void init_framebuffer(void) {
    log_msg("init_framebuffer: opening /dev/fb0");
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        log_msg("init_framebuffer: FAILED to open /dev/fb0");
        perror("Failed to open framebuffer");
        exit(1);
    }
    log_msg("init_framebuffer: /dev/fb0 opened");

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        log_msg("init_framebuffer: FAILED to get screen info");
        perror("Failed to get screen info");
        close(fb_fd);
        exit(1);
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        log_msg("init_framebuffer: FAILED to get fixed screen info");
        perror("Failed to get fixed screen info");
        close(fb_fd);
        exit(1);
    }

    screen_width = vinfo.xres;
    screen_height = vinfo.yres;
    bytes_per_pixel = vinfo.bits_per_pixel / 8;

    if (bytes_per_pixel < 1) bytes_per_pixel = 1;

    size_t mem_size = finfo.smem_len;
    char buf[128];
    snprintf(buf, sizeof(buf), "init_framebuffer: %dx%d %d bpp, line_length=%d, smem_len=%zu",
             screen_width, screen_height, vinfo.bits_per_pixel, finfo.line_length, mem_size);
    log_msg(buf);

    fb_mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        log_msg("init_framebuffer: FAILED to mmap");
        perror("Failed to mmap framebuffer");
        close(fb_fd);
        exit(1);
    }
    log_msg("init_framebuffer: mmap OK");
}

void init_input(void) {
    log_msg("init_input: scanning /dev/input");
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        log_msg("init_input: cannot open /dev/input");
        fprintf(stderr, "Cannot open /dev/input\n");
        return;
    }

    struct dirent *entry;
    int best_fd = -1;
    int best_score = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[256] = "unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        int score = 0;
        if (strstr(name, "touch") || strstr(name, "Touch") || strstr(name, "TOUCH")) score += 10;
        if (strstr(name, "multi") || strstr(name, "Multi")) score += 5;
        if (strstr(name, "mt") || strstr(name, "MT")) score += 3;
        if (strstr(name, "Kindle")) score += 2;

        printf("Input %s: '%s' (score=%d)\n", entry->d_name, name, score);
        char buf[256];
        snprintf(buf, sizeof(buf), "init_input: %s = '%s' score=%d", entry->d_name, name, score);
        log_msg(buf);

        if (score > best_score) {
            if (best_fd >= 0) close(best_fd);
            best_fd = fd;
            best_score = score;
            strcpy(path, path);
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (best_fd >= 0) {
        input_fd = best_fd;
        log_msg("init_input: using best device");
    } else {
        log_msg("init_input: NO input device found");
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
    release_lock();
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

    if (c == ' ') char_index = 0;
    else if (c == '!') char_index = 1;
    else if (c >= 'A' && c <= 'Z') char_index = 2 + (c - 'A');
    else if (c >= 'a' && c <= 'z') char_index = 2 + (c - 'a');
    else if (c >= '0' && c <= '9') char_index = 28 + (c - '0');
    else if (c == '.') char_index = 1;
    else if (c == ':') char_index = 1;
    else if (c == '-') char_index = 1;
    else if (c == '/') char_index = 1;
    else if (c == '[') char_index = 11;
    else if (c == ']') char_index = 11;
    else return;

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

void draw_text_centered(int y, const char *text, unsigned char color, int scale) {
    int len = strlen(text);
    int text_w = len * 5 * scale + (len - 1) * scale;
    int x = (screen_width - text_w) / 2;
    draw_text(x, y, text, color, scale);
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

    const char *cmd = "curl -s --connect-timeout 5 --max-time 10 " GITHUB_API_URL " 2>/dev/null";
    FILE *fp = popen(cmd, "r");
    if (fp == NULL) return -1;

    char buffer[4096];
    char tag_name[32] = "";
    char body[1024] = "";
    int in_body = 0;
    int body_idx = 0;

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
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

    if (strlen(tag_name) > 0) {
        char *ver = tag_name;
        if (ver[0] == 'v') ver++;
        strcpy(latest_version, ver);
        strncpy(update_notes, body, sizeof(update_notes) - 1);

        printf("Current: %s, Latest: %s\n", KINDLEJAP_VERSION, latest_version);

        if (compare_versions(KINDLEJAP_VERSION, latest_version) < 0) {
            return 1;
        } else {
            return 0;
        }
    }

    return -1;
}

void show_splash(void) {
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);

    draw_text_centered(screen_height / 2 - 40, "KINDLEJAP", COLOR_BLACK, 4);
    draw_text_centered(screen_height / 2 + 20, "v" KINDLEJAP_VERSION, COLOR_GRAY, 2);

    draw_text_centered(screen_height / 2 + 60, "LAUNCHER FOR KINDLE", COLOR_DARK, 1);
}

void show_init_status(const char *msg) {
    static int y_pos = 0;
    if (y_pos == 0) y_pos = screen_height / 2 + 100;

    draw_text(50, y_pos, msg, COLOR_DARK, 1);
    y_pos += 12;
}

void show_splash_init(void) {
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);

    draw_text_centered(40, "KINDLEJAP", COLOR_BLACK, 4);
    draw_text_centered(90, "v" KINDLEJAP_VERSION, COLOR_GRAY, 2);
    draw_rect(screen_width / 2 - 150, 115, 300, 2, COLOR_DARK);

    draw_text_centered(140, "INITIALIZING...", COLOR_DARK, 2);

    draw_text(50, 190, "Testing framebuffer...", COLOR_DARK, 1);
    draw_text(50, 210, "[OK] Framebuffer ready", COLOR_BLACK, 1);

    draw_text(50, 240, "Testing input devices...", COLOR_DARK, 1);
}

void draw_taskbar(void) {
    int taskbar_y = screen_height - TASKBAR_HEIGHT;
    draw_rect(0, taskbar_y, screen_width, TASKBAR_HEIGHT, COLOR_LIGHT);
    draw_rect(0, taskbar_y, screen_width, 2, COLOR_DARK);

    int btn_x = 10;
    int btn_y = taskbar_y + 10;
    int btn_w = MENU_BUTTON_WIDTH;
    int btn_h = TASKBAR_HEIGHT - 20;

    draw_rect(btn_x, btn_y, btn_w, btn_h, COLOR_GRAY);
    int text_w = 4 * 5;
    int text_x = btn_x + (btn_w - text_w) / 2;
    int text_y = btn_y + (btn_h - 7) / 2;
    draw_text(text_x, text_y, "MENU", COLOR_BLACK, 1);

    draw_text(screen_width - 200, taskbar_y + 20, "KindleJap", COLOR_BLACK, 2);
}

void draw_menu(void) {
    if (!menu_expanded) return;

    int menu_width = 250;
    int menu_height = 200;
    int menu_x = 10;
    int menu_y = screen_height - TASKBAR_HEIGHT - menu_height - 10;

    draw_rect(menu_x, menu_y, menu_width, menu_height, COLOR_WHITE);
    draw_rect(menu_x, menu_y, menu_width, 2, COLOR_BLACK);
    draw_rect(menu_x, menu_y + menu_height - 2, menu_width, 2, COLOR_BLACK);
    draw_rect(menu_x, menu_y, 2, menu_height, COLOR_BLACK);
    draw_rect(menu_x + menu_width - 2, menu_y, 2, menu_height, COLOR_BLACK);

    int item_height = 40;
    int item_y = menu_y + 10;

    draw_rect(menu_x + 10, item_y, menu_width - 20, item_height, COLOR_LIGHT);
    draw_text(menu_x + 20, item_y + 15, "Apps", COLOR_BLACK, 2);
    menu_items[0].x = menu_x + 10;
    menu_items[0].y = item_y;
    menu_items[0].w = menu_width - 20;
    menu_items[0].h = item_height;
    menu_items[0].active = 1;
    item_y += item_height + 5;

    draw_rect(menu_x + 10, item_y, menu_width - 20, item_height, COLOR_LIGHT);
    draw_text(menu_x + 20, item_y + 15, "Settings", COLOR_BLACK, 2);
    menu_items[1].x = menu_x + 10;
    menu_items[1].y = item_y;
    menu_items[1].w = menu_width - 20;
    menu_items[1].h = item_height;
    menu_items[1].active = 1;
    item_y += item_height + 5;

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

    int dlg_width = 400;
    int dlg_height = 250;
    int dlg_x = (screen_width - dlg_width) / 2;
    int dlg_y = (screen_height - dlg_height) / 2;

    draw_rect(0, 0, screen_width, screen_height, COLOR_DARK);
    draw_rect(dlg_x, dlg_y, dlg_width, dlg_height, COLOR_WHITE);
    draw_rect(dlg_x, dlg_y, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y + dlg_height - 2, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y, 2, dlg_height, COLOR_BLACK);
    draw_rect(dlg_x + dlg_width - 2, dlg_y, 2, dlg_height, COLOR_BLACK);

    draw_text(dlg_x + 20, dlg_y + 20, "UPDATE AVAILABLE", COLOR_BLACK, 2);

    char version_text[64];
    snprintf(version_text, sizeof(version_text), "Current: %s", KINDLEJAP_VERSION);
    draw_text(dlg_x + 20, dlg_y + 50, version_text, COLOR_BLACK, 1);

    snprintf(version_text, sizeof(version_text), "Latest:  %s", latest_version);
    draw_text(dlg_x + 20, dlg_y + 65, version_text, COLOR_BLACK, 1);

    draw_text(dlg_x + 20, dlg_y + 90, "New version available!", COLOR_BLACK, 1);

    int btn_w = 120;
    int btn_h = 40;
    int btn_y = dlg_y + dlg_height - 60;

    draw_rect(dlg_x + 60, btn_y, btn_w, btn_h, COLOR_GRAY);
    draw_text(dlg_x + 80, btn_y + 15, "UPDATE", COLOR_BLACK, 2);

    draw_rect(dlg_x + 220, btn_y, btn_w, btn_h, COLOR_GRAY);
    draw_text(dlg_x + 250, btn_y + 15, "CANCEL", COLOR_BLACK, 2);

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

    int dlg_width = 400;
    int dlg_height = 150;
    int dlg_x = (screen_width - dlg_width) / 2;
    int dlg_y = (screen_height - dlg_height) / 2;

    draw_rect(0, 0, screen_width, screen_height, COLOR_DARK);
    draw_rect(dlg_x, dlg_y, dlg_width, dlg_height, COLOR_WHITE);
    draw_rect(dlg_x, dlg_y, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y + dlg_height - 2, dlg_width, 2, COLOR_BLACK);
    draw_rect(dlg_x, dlg_y, 2, dlg_height, COLOR_BLACK);
    draw_rect(dlg_x + dlg_width - 2, dlg_y, 2, dlg_height, COLOR_BLACK);

    draw_text(dlg_x + 20, dlg_y + 20, "UPDATING...", COLOR_BLACK, 2);

    int bar_x = dlg_x + 20;
    int bar_y = dlg_y + 70;
    int bar_w = dlg_width - 40;
    int bar_h = 20;

    draw_rect(bar_x, bar_y, bar_w, bar_h, COLOR_LIGHT);

    if (update_progress > 0) {
        int fill_w = (bar_w * update_progress) / 100;
        draw_rect(bar_x, bar_y, fill_w, bar_h, COLOR_DARK);
    }

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
}

void action_settings(void) {
    printf("Opening Settings...\n");
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

    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
    draw_taskbar();
    draw_text(100, screen_height / 2 - 20, "Checking for updates...", COLOR_BLACK, 2);
    redraw_screen();

    int result = check_for_updates();

    if (result == 1) {
        show_update_dialog = 1;
        update_available = 1;
    } else if (result == 0) {
        draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
        draw_taskbar();
        draw_text(100, screen_height / 2 - 20, "Already up to date!", COLOR_BLACK, 2);
        redraw_screen();
        sleep(2);
    } else {
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

void *download_and_update_thread(void *arg) {
    (void)arg;
    printf("Starting update...\n");
    update_downloading = 1;
    update_progress = 0;
    redraw_screen();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s update", UPDATE_SCRIPT, KINDLEJAP_VERSION);
    int ret = system(cmd);

    update_downloading = 0;

    if (ret == 0) {
        printf("Update completed!\n");
        update_progress = 100;
        redraw_screen();
        sleep(2);
        execl(UPDATE_SCRIPT, UPDATE_SCRIPT, "restart", NULL);
    } else {
        printf("Update failed!\n");
        update_progress = -1;
        redraw_screen();
        sleep(2);
    }
    return NULL;
}

void action_update_now(void) {
    printf("Starting update...\n");
    menu_expanded = 0;
    show_update_dialog = 0;
    update_downloading = 1;
    update_progress = 0;
    redraw_screen();

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
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
    draw_taskbar();
    draw_menu();
    draw_update_dialog();
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

        if (y > screen_height - TASKBAR_HEIGHT - 50) {
            swipe_detected = 1;
        }
    } else {
        int dx = x - touch_start_x;
        int dy = y - touch_start_y;

        if (swipe_detected && dy > 50 && abs(dx) < 30) {
            menu_expanded = !menu_expanded;
            redraw_screen();
            return;
        }

        if (x >= 10 && x <= 10 + MENU_BUTTON_WIDTH &&
            y >= screen_height - TASKBAR_HEIGHT + 10 &&
            y <= screen_height - 10) {
            menu_expanded = !menu_expanded;
            redraw_screen();
            return;
        }

        if (menu_expanded) {
            for (int i = 0; i < 3; i++) {
                if (menu_items[i].active &&
                    x >= menu_items[i].x && x <= menu_items[i].x + menu_items[i].w &&
                    y >= menu_items[i].y && y <= menu_items[i].y + menu_items[i].h) {
                    switch (i) {
                        case 0: action_apps(); break;
                        case 1: action_settings(); break;
                        case 2: action_exit(); break;
                    }
                    menu_expanded = 0;
                    redraw_screen();
                    return;
                }
            }
        }

        if (show_update_dialog) {
            if (x >= menu_items[3].x && x <= menu_items[3].x + menu_items[3].w &&
                y >= menu_items[3].y && y <= menu_items[3].y + menu_items[3].h) {
                action_update_yes();
                redraw_screen();
                return;
            }

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
            } else if (ev.code == ABS_X) {
                touch_last_x = ev.value;
            } else if (ev.code == ABS_Y) {
                touch_last_y = ev.value;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            handle_touch(touch_last_x, touch_last_y, ev.value);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
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

    log_msg("=== KindleJap v" KINDLEJAP_VERSION " ===");
    log_msg("main: starting");
    log_msg("main: log file: " LOGFILE);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        log_msg("main: another instance running, killing it");
        system("killall kindlejap-bin 2>/dev/null");
        sleep(1);
        lock_fd = acquire_lock();
        if (lock_fd < 0) {
            log_msg("main: FAILED to acquire lock");
            fprintf(stderr, "Failed to acquire lock\n");
            return 1;
        }
    }

    log_msg("main: init_framebuffer");
    init_framebuffer();

    log_msg("main: show_splash_init");
    show_splash_init();

    log_msg("main: init_input");
    init_input();

    if (input_fd >= 0) {
        draw_text(50, 260, "[OK] Input device found", COLOR_BLACK, 1);
    } else {
        draw_text(50, 260, "[WARN] No input device", COLOR_DARK, 1);
    }

    draw_text(50, 290, "Checking for updates...", COLOR_DARK, 1);
    draw_text(50, 310, "Starting launcher...", COLOR_DARK, 1);

    menu_items[0].action = action_apps;
    menu_items[1].action = action_settings;
    menu_items[2].action = action_exit;
    menu_items[3].action = action_update_yes;
    menu_items[4].action = action_update_no;

    usleep(500000);

    log_msg("main: redraw_screen");
    redraw_screen();

    pthread_t update_check_thread;
    pthread_create(&update_check_thread, NULL, check_updates_thread, NULL);
    pthread_detach(update_check_thread);

    log_msg("main: entering main loop");

    while (running) {
        if (input_fd >= 0) {
            process_input();
        }
        usleep(50000);
    }

    log_msg("main: exiting");
    cleanup();

    if (logfp) fclose(logfp);
    printf("KindleJap exited.\n");
    return 0;
}
