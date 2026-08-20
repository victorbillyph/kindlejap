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
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>

#define KINDLEJAP_VERSION "2.0.0"
#define GITHUB_API_URL "https://api.github.com/repos/victorbillyph/kindlejap/releases/latest"
#define UPDATE_SCRIPT "/mnt/us/extensions/kindlejap/bin/update.sh"
#define LOCKFILE "/tmp/kindlejap.lock"
#define LOGFILE "/mnt/us/kindlejap.log"
#define APPS_DIR "/mnt/us/extensions/kindlejap/apps"
#define COMMUNITY_APPS_DIR "/mnt/us/kindlejap_apps"

#define TASKBAR_H 56
#define KEYBOARD_H 340
#define COLOR_BLACK 0x00
#define COLOR_WHITE 0xFF
#define COLOR_GRAY 0x80
#define COLOR_LIGHT 0xC0
#define COLOR_DARK 0x40
#define COLOR_SELECT 0x60

static int log_fd = -1;
void log_msg(const char *msg) {
    if (log_fd < 0) log_fd = open(LOGFILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd >= 0) { write(log_fd, msg, strlen(msg)); write(log_fd, "\n", 1); fsync(log_fd); }
}

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
int fb_fd = -1;
unsigned char *fb_mem = NULL;
int screen_width, screen_height, bytes_per_pixel;
int input_fd = -1;
volatile int running = 1;
int lock_fd = -1;

struct mxcfb_rect { uint32_t top, left, width, height; };
struct mxcfb_alt_buffer_data { uint32_t phys_addr, width, height; struct mxcfb_rect alt_update_region; };
struct mxcfb_update_data {
    struct mxcfb_rect update_region; uint32_t waveform_mode, update_mode, update_marker;
    uint32_t hist_bw_waveform_mode, hist_gray_waveform_mode; int temp; unsigned int flags;
    struct mxcfb_alt_buffer_data alt_buffer_data;
};
struct mxcfb_update_data_zelda {
    struct mxcfb_rect update_region; uint32_t waveform_mode, update_mode, update_marker;
    int temp; unsigned int flags; int dither_mode, quant_bit;
    struct mxcfb_alt_buffer_data alt_buffer_data;
    uint32_t hist_bw_waveform_mode, hist_gray_waveform_mode, ts_pxp, ts_epdc;
};
struct mxcfb_update_marker_data { uint32_t update_marker, collision_test; };

#define MXCFB_SEND_UPDATE_K51     1078478382
#define MXCFB_SEND_UPDATE_ZELDA   1079526958
#define MXCFB_WAIT_COMPLETE_CARTA 3221767727
#define MXCFB_WAIT_COMPLETE_PEARL 1074021935
#define WFM_GC16 2
#define WFM_GC16_FAST 3
#define UPD_PARTIAL 0
#define UPD_FULL 1

static uint32_t refresh_marker = 0;
static int mxcfb_mode = 0;

static int do_refresh(int full, int wfm) {
    if (fb_fd < 0) return -1;
    refresh_marker++;
    if (refresh_marker > 0xFFFFFFFF) refresh_marker = 1;
    if (mxcfb_mode == 0 || mxcfb_mode == 1) {
        struct mxcfb_update_data d;
        memset(&d, 0, sizeof(d));
        d.update_region.width = screen_width; d.update_region.height = screen_height;
        d.waveform_mode = wfm; d.update_mode = full ? UPD_FULL : UPD_PARTIAL;
        d.update_marker = refresh_marker; d.hist_bw_waveform_mode = 1;
        d.hist_gray_waveform_mode = full ? WFM_GC16 : WFM_GC16_FAST;
        d.temp = 4097; if (ioctl(fb_fd, MXCFB_SEND_UPDATE_K51, &d) == 0) {
            mxcfb_mode = 1; struct mxcfb_update_marker_data md = {refresh_marker, 0};
            ioctl(fb_fd, MXCFB_WAIT_COMPLETE_CARTA, &md); return 0;
        }
        struct mxcfb_update_data_zelda dz;
        memset(&dz, 0, sizeof(dz));
        dz.update_region.width = screen_width; dz.update_region.height = screen_height;
        dz.waveform_mode = wfm; dz.update_mode = full ? UPD_FULL : UPD_PARTIAL;
        dz.update_marker = refresh_marker; dz.temp = 4096; dz.flags = 0;
        dz.hist_bw_waveform_mode = 1; dz.hist_gray_waveform_mode = full ? WFM_GC16 : WFM_GC16_FAST;
        if (ioctl(fb_fd, MXCFB_SEND_UPDATE_ZELDA, &dz) == 0) {
            mxcfb_mode = 2; struct mxcfb_update_marker_data md = {refresh_marker, 0};
            ioctl(fb_fd, MXCFB_WAIT_COMPLETE_CARTA, &md); return 0;
        }
    }
    return -1;
}
void refresh_screen(void) { do_refresh(1, WFM_GC16); }
void refresh_screen_partial(void) { do_refresh(0, WFM_GC16_FAST); }

int acquire_lock(void) {
    lock_fd = open(LOCKFILE, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) return -1;
    struct flock fl = {F_WRLCK, SEEK_SET, 0, 0};
    if (fcntl(lock_fd, F_SETLK, &fl) < 0) { close(lock_fd); lock_fd = -1; return -1; }
    char s[16]; snprintf(s, sizeof(s), "%d", getpid());
    ftruncate(lock_fd, 0); write(lock_fd, s, strlen(s));
    return 0;
}
void release_lock(void) { if (lock_fd >= 0) { close(lock_fd); lock_fd = -1; } unlink(LOCKFILE); }

void restore_kindle_ui(void) {
    system("initctl start lab126_gui 2>/dev/null");
    system("initctl start otaupd 2>/dev/null"); system("initctl start phd 2>/dev/null");
    system("initctl start tmd 2>/dev/null"); system("initctl start todo 2>/dev/null");
    system("initctl start mcsd 2>/dev/null");
    system("lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null");
}

void init_framebuffer(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { log_msg("FB open failed"); exit(1); }
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    screen_width = vinfo.xres; screen_height = vinfo.yres;
    bytes_per_pixel = vinfo.bits_per_pixel / 8;
    if (bytes_per_pixel < 1) bytes_per_pixel = 1;
    fb_mem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { log_msg("mmap failed"); exit(1); }
    char buf[128]; snprintf(buf, sizeof(buf), "FB: %dx%d %dbpp", screen_width, screen_height, vinfo.bits_per_pixel);
    log_msg(buf);
}

void init_input(void) {
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e; int best = -1, best_s = -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[64]; snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        char name[256] = ""; ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        int s = 0;
        if (strstr(name, "touch") || strstr(name, "Touch")) s += 10;
        if (strstr(name, "multi") || strstr(name, "Multi")) s += 5;
        if (s > best_s) { if (best >= 0) close(best); best = fd; best_s = s; }
        else close(fd);
    }
    closedir(d);
    input_fd = best;
}

void draw_rect(int x, int y, int w, int h, unsigned char c) {
    for (int j = y; j < y + h && j < screen_height; j++)
        for (int i = x; i < x + w && i < screen_width; i++) {
            int off = j * finfo.line_length + i * bytes_per_pixel;
            if (off >= 0 && off < (int)finfo.smem_len) { fb_mem[off] = c; if (bytes_per_pixel > 1) fb_mem[off+1] = c; }
        }
}

static const unsigned char font5x7[][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    {0x08,0x14,0x22,0x22,0x3E,0x22,0x22},{0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C},
    {0x1C,0x22,0x20,0x20,0x20,0x22,0x1C},{0x18,0x24,0x22,0x22,0x22,0x24,0x18},
    {0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E},{0x3E,0x20,0x20,0x3C,0x20,0x20,0x20},
    {0x1C,0x22,0x20,0x2E,0x22,0x22,0x1C},{0x22,0x22,0x22,0x3E,0x22,0x22,0x22},
    {0x1C,0x08,0x08,0x08,0x08,0x08,0x1C},{0x02,0x02,0x02,0x02,0x02,0x22,0x1C},
    {0x22,0x24,0x28,0x30,0x28,0x24,0x22},{0x20,0x20,0x20,0x20,0x20,0x20,0x3E},
    {0x22,0x36,0x2A,0x2A,0x22,0x22,0x22},{0x22,0x32,0x2A,0x26,0x22,0x22,0x22},
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C},{0x1C,0x22,0x22,0x1C,0x20,0x20,0x20},
    {0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A},{0x1C,0x22,0x22,0x1C,0x28,0x24,0x22},
    {0x1C,0x22,0x20,0x1C,0x02,0x22,0x1C},{0x3E,0x08,0x08,0x08,0x08,0x08,0x08},
    {0x22,0x22,0x22,0x22,0x22,0x22,0x1C},{0x22,0x22,0x22,0x22,0x22,0x14,0x08},
    {0x22,0x22,0x22,0x2A,0x2A,0x36,0x22},{0x22,0x22,0x14,0x08,0x14,0x22,0x22},
    {0x22,0x22,0x14,0x08,0x08,0x08,0x08},{0x3E,0x02,0x04,0x08,0x10,0x20,0x3E},
    {0x08,0x14,0x22,0x22,0x3E,0x22,0x22},{0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C},
    {0x1C,0x22,0x20,0x20,0x20,0x22,0x1C},{0x18,0x24,0x22,0x22,0x22,0x24,0x18},
    {0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E},{0x3E,0x20,0x20,0x3C,0x20,0x20,0x20},
    {0x1C,0x22,0x20,0x2E,0x22,0x22,0x1C},{0x22,0x22,0x22,0x3E,0x22,0x22,0x22},
    {0x1C,0x08,0x08,0x08,0x08,0x08,0x1C},{0x02,0x02,0x02,0x02,0x02,0x22,0x1C},
    {0x22,0x24,0x28,0x30,0x28,0x24,0x22},{0x20,0x20,0x20,0x20,0x20,0x20,0x3E},
    {0x22,0x36,0x2A,0x2A,0x22,0x22,0x22},{0x22,0x32,0x2A,0x26,0x22,0x22,0x22},
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C},{0x1C,0x22,0x22,0x1C,0x20,0x20,0x20},
    {0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A},{0x1C,0x22,0x22,0x1C,0x28,0x24,0x22},
    {0x1C,0x22,0x20,0x1C,0x02,0x22,0x1C},{0x3E,0x08,0x08,0x08,0x08,0x08,0x08},
    {0x22,0x22,0x22,0x22,0x22,0x22,0x1C},{0x22,0x22,0x22,0x22,0x22,0x14,0x08},
    {0x22,0x22,0x22,0x2A,0x2A,0x36,0x22},{0x22,0x22,0x14,0x08,0x14,0x22,0x22},
    {0x22,0x22,0x14,0x08,0x08,0x08,0x08},{0x3E,0x02,0x04,0x08,0x10,0x20,0x3E},
    {0x1C,0x22,0x26,0x2A,0x32,0x22,0x1C},{0x08,0x18,0x08,0x08,0x08,0x08,0x1C},
    {0x1C,0x22,0x02,0x0C,0x10,0x20,0x3E},{0x3E,0x02,0x04,0x0C,0x02,0x22,0x1C},
    {0x04,0x0C,0x14,0x24,0x3E,0x04,0x04},{0x3E,0x20,0x3C,0x02,0x02,0x22,0x1C},
    {0x0C,0x10,0x20,0x3C,0x22,0x22,0x1C},{0x3E,0x02,0x04,0x08,0x10,0x10,0x10},
    {0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C},{0x1C,0x22,0x22,0x1E,0x02,0x04,0x18},
};

void draw_char(int x, int y, char c, unsigned char color, int scale) {
    int idx = 0;
    if (c >= 'A' && c <= 'Z') idx = 2 + (c - 'A');
    else if (c >= 'a' && c <= 'z') idx = 2 + (c - 'a');
    else if (c >= '0' && c <= '9') idx = 28 + (c - '0');
    else if (c == '+') idx = 41; else if (c == '-') idx = 42;
    else if (c == '*') idx = 43; else if (c == '/') idx = 44;
    else if (c == '=') idx = 45; else if (c == '.') idx = 46;
    else if (c == ',') idx = 47; else if (c == '(') idx = 48;
    else if (c == ')') idx = 49; else if (c == '_') idx = 50;
    else if (c == ':') idx = 51; else if (c == ';') idx = 52;
    else if (c == '<') idx = 53; else if (c == '>') idx = 54;
    else if (c == '?') idx = 55; else if (c == '!') idx = 56;
    else if (c == '#') idx = 57; else if (c == '@') idx = 58;
    else if (c == '%') idx = 59; else if (c == '^') idx = 60;
    else if (c == '|') idx = 61; else if (c == '~') idx = 62;
    else if (c == '[') idx = 63; else if (c == ']') idx = 64;
    else if (c == '{') idx = 65; else if (c == '}') idx = 66;
    else return;
    for (int row = 0; row < 7; row++) {
        unsigned char bits = font5x7[idx][row];
        for (int col = 0; col < 5; col++)
            if (bits & (0x20 >> col))
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col*scale + sx, py = y + row*scale + sy;
                        if (px < screen_width && py < screen_height) {
                            int off = py * finfo.line_length + px * bytes_per_pixel;
                            if (off >= 0 && off < (int)finfo.smem_len) { fb_mem[off] = color; if (bytes_per_pixel>1) fb_mem[off+1]=color; }
                        }
                    }
    }
}

void draw_text(int x, int y, const char *t, unsigned char c, int s) {
    while (*t) { draw_char(x, y, *t, c, s); x += 6*s; t++; }
}
void draw_text_centered(int y, const char *t, unsigned char c, int s) {
    int l = strlen(t); int w = l * 6 * s;
    draw_text((screen_width - w) / 2, y, t, c, s);
}
void draw_text_right(int y, const char *t, unsigned char c, int s) {
    int l = strlen(t); int w = l * 6 * s;
    draw_text(screen_width - w - 10, y, t, c, s);
}
int text_width(const char *t, int s) { return strlen(t) * 6 * s; }

int point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x+w && py >= y && py < y+h;
}

/* ========================= KEYBOARD ========================= */
#define KB_ROWS 5
#define KB_KEY_H 52
#define KB_KEY_PAD 4

typedef void (*keyboard_cb)(const char *text);
static int keyboard_visible = 0;
static int keyboard_shift = 0;
static char keyboard_buffer[256] = "";
static int keyboard_buf_len = 0;
static keyboard_cb keyboard_callback = NULL;
static int menu_visible = 0;

static const char *kb_layout_lower[KB_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl;",
    "zxcvbnm,.",
    " "
};
static const char *kb_layout_upper[KB_ROWS] = {
    "!@#$%^&*()",
    "QWERTYUIOP",
    "ASDFGHJKL:",
    "ZXCVBNM<>.",
    " "
};
static const int kb_col_count[KB_ROWS] = {10, 10, 9, 9, 1};

static int kb_start_y(void) { return screen_height - KEYBOARD_H; }

void keyboard_show(keyboard_cb cb, const char *initial) {
    keyboard_visible = 1; keyboard_shift = 0; keyboard_callback = cb;
    keyboard_buf_len = 0; keyboard_buffer[0] = 0;
    if (initial) { strncpy(keyboard_buffer, initial, sizeof(keyboard_buffer)-1); keyboard_buf_len = strlen(keyboard_buffer); }
}

void keyboard_hide(void) { keyboard_visible = 0; keyboard_callback = NULL; }

void keyboard_draw(void) {
    if (!keyboard_visible) return;
    int ky = kb_start_y();
    draw_rect(0, ky, screen_width, KEYBOARD_H, COLOR_LIGHT);
    draw_rect(0, ky, screen_width, 2, COLOR_DARK);
    draw_text(10, ky+6, keyboard_buffer, COLOR_BLACK, 2);
    int bx = screen_width - 100;
    draw_rect(bx, ky+4, 90, 30, COLOR_DARK);
    draw_text(bx+10, ky+10, "CLOSE", COLOR_WHITE, 2);

    const char **layout = keyboard_shift ? kb_layout_upper : kb_layout_lower;
    int total_w = screen_width - 20;
    for (int r = 0; r < KB_ROWS; r++) {
        int cols = kb_col_count[r];
        int row_y = ky + 42 + r * (KB_KEY_H + KB_KEY_PAD);
        if (r == KB_ROWS - 1) {
            draw_rect(10, row_y, 80, KB_KEY_H, COLOR_DARK);
            draw_text(20, row_y+18, "SHIFT", keyboard_shift ? COLOR_BLACK : COLOR_WHITE, 2);
            int kw = total_w - 200;
            int kx = 100;
            draw_rect(kx, row_y, kw, KB_KEY_H, COLOR_WHITE);
            draw_text_centered(row_y+18, "SPACE", COLOR_DARK, 2);
            draw_rect(screen_width - 90, row_y, 80, KB_KEY_H, COLOR_DARK);
            draw_text(screen_width - 80, row_y+18, "BS", COLOR_WHITE, 2);
        } else {
            int kw = (total_w - (cols-1)*KB_KEY_PAD) / cols;
            for (int c = 0; c < cols; c++) {
                int kx = 10 + c * (kw + KB_KEY_PAD);
                char ch = layout[r][c];
                draw_rect(kx, row_y, kw, KB_KEY_H, COLOR_WHITE);
                char s[2] = {ch, 0};
                int sw = text_width(s, 2);
                draw_text(kx + (kw-sw)/2, row_y+18, s, COLOR_BLACK, 2);
            }
        }
    }
}

void keyboard_handle_touch(int x, int y) {
    if (!keyboard_visible) return;
    int ky = kb_start_y();
    if (y < ky) return;

    int bx = screen_width - 100;
    if (point_in_rect(x, y, bx, ky+4, 90, 30)) { keyboard_submit(); return; }

    int row_y_start = ky + 42;
    int total_w = screen_width - 20;
    for (int r = 0; r < KB_ROWS; r++) {
        int row_y = row_y_start + r * (KB_KEY_H + KB_KEY_PAD);
        if (r == KB_ROWS - 1) {
            if (point_in_rect(x, y, 10, row_y, 80, KB_KEY_H)) {
                keyboard_shift = !keyboard_shift; return;
            }
            int kw = total_w - 200;
            int kx = 100;
            if (point_in_rect(x, y, kx, row_y, kw, KB_KEY_H)) {
                if (keyboard_buf_len < (int)sizeof(keyboard_buffer)-1) {
                    keyboard_buffer[keyboard_buf_len++] = ' ';
                    keyboard_buffer[keyboard_buf_len] = 0;
                }
                return;
            }
            if (point_in_rect(x, y, screen_width-90, row_y, 80, KB_KEY_H)) {
                if (keyboard_buf_len > 0) keyboard_buffer[--keyboard_buf_len] = 0;
                return;
            }
        } else {
            int cols = kb_col_count[r];
            int kw = (total_w - (cols-1)*KB_KEY_PAD) / cols;
            const char **layout = keyboard_shift ? kb_layout_upper : kb_layout_lower;
            for (int c = 0; c < cols; c++) {
                int kx = 10 + c * (kw + KB_KEY_PAD);
                if (point_in_rect(x, y, kx, row_y, kw, KB_KEY_H)) {
                    char ch = layout[r][c];
                    if (ch && keyboard_buf_len < (int)sizeof(keyboard_buffer)-1) {
                        keyboard_buffer[keyboard_buf_len++] = ch;
                        keyboard_buffer[keyboard_buf_len] = 0;
                    }
                    return;
                }
            }
        }
    }
}

void keyboard_submit(void) {
    if (keyboard_callback) keyboard_callback(keyboard_buffer);
    keyboard_hide();
}

/* ========================= APP FRAMEWORK ========================= */
#define MAX_OPEN_APPS 16
#define MAX_REGISTERED_APPS 32

typedef struct App {
    char name[32];
    char icon[8];
    void (*init)(void);
    void (*draw)(int x, int y, int w, int h);
    void (*on_touch)(int x, int y, int pressed);
    void (*cleanup)(void);
    void *data;
} App;

static App registered[MAX_REGISTERED_APPS];
static int registered_count = 0;
static App *open_apps[MAX_OPEN_APPS];
static int open_count = 0;
static int active_app_idx = -1;

void app_register(App *app) {
    if (registered_count < MAX_REGISTERED_APPS) registered[registered_count++] = *app;
}

void app_open(App *app) {
    for (int i = 0; i < open_count; i++) if (open_apps[i] == app) { active_app_idx = i; return; }
    if (open_count >= MAX_OPEN_APPS) return;
    open_apps[open_count] = app;
    active_app_idx = open_count;
    open_count++;
    if (app->init) app->init();
}

void app_close(int idx) {
    if (idx < 0 || idx >= open_count) return;
    if (open_apps[idx]->cleanup) open_apps[idx]->cleanup();
    for (int i = idx; i < open_count - 1; i++) open_apps[i] = open_apps[i+1];
    open_count--;
    if (active_app_idx >= open_count) active_app_idx = open_count - 1;
}

static int close_btn_mode = 0;
static int close_btn_target = -1;
static int long_press_x = -1, long_press_y = -1;
static unsigned long long press_start_time = 0;

void taskbar_draw(void) {
    int ty = screen_height - TASKBAR_H;
    draw_rect(0, ty, screen_width, TASKBAR_H, COLOR_LIGHT);
    draw_rect(0, ty, screen_width, 2, COLOR_DARK);
    draw_rect(0, ty+2, 70, TASKBAR_H-2, COLOR_DARK);
    draw_text(12, ty+18, "MENU", COLOR_WHITE, 2);

    int tx = 80;
    for (int i = 0; i < open_count; i++) {
        int tw = text_width(open_apps[i]->name, 2) + 20;
        if (i == active_app_idx) draw_rect(tx, ty+2, tw, TASKBAR_H-2, COLOR_SELECT);
        draw_rect(tx, ty+2, tw, TASKBAR_H-2, COLOR_WHITE);
        if (i == active_app_idx) draw_rect(tx, ty+2, tw, TASKBAR_H-2, COLOR_SELECT);
        draw_rect(tx, ty+2, 3, TASKBAR_H-2, COLOR_DARK);
        draw_text(tx+10, ty+18, open_apps[i]->name, COLOR_BLACK, 2);
        if (close_btn_mode && close_btn_target == i) {
            draw_rect(tx+tw-4, ty+8, 24, TASKBAR_H-16, COLOR_DARK);
            draw_text(tx+tw-1, ty+16, "X", COLOR_WHITE, 2);
        }
        tx += tw + 4;
    }
}

int taskbar_handle_touch(int x, int y, int pressed) {
    int ty = screen_height - TASKBAR_H;
    if (y < ty) return 0;
    if (x < 70) {
        if (!pressed) {
            menu_visible = !menu_visible;
            return 1;
        }
        return 1;
    }
    int tx = 80;
    for (int i = 0; i < open_count; i++) {
        int tw = text_width(open_apps[i]->name, 2) + 20;
        if (point_in_rect(x, y, tx, ty+2, tw, TASKBAR_H-2)) {
            if (pressed) {
                long_press_x = x; long_press_y = y;
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                press_start_time = ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
                close_btn_mode = 0; close_btn_target = -1;
            } else {
                if (close_btn_mode && close_btn_target == i) { app_close(i); close_btn_mode = 0; close_btn_target = -1; return 1; }
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                unsigned long long now = ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
                if (now - press_start_time > 500) { close_btn_mode = 1; close_btn_target = i; return 1; }
                active_app_idx = i; return 1;
            }
        }
        tx += tw + 4;
    }
    return 0;
}

/* ========================= MENU ========================= */

void menu_draw(void) {
    if (!menu_visible) return;
    draw_rect(0, 0, screen_width, screen_height - TASKBAR_H, 0x20);
    int y = 100;
    draw_text_centered(60, "KINDLEJAP " KINDLEJAP_VERSION, COLOR_WHITE, 3);
    const char *items[] = {"Calculator", "File Explorer", "Network", "Browser", "KUAL Apps", "Settings", "Exit"};
    int count = 7;
    for (int i = 0; i < count; i++) {
        int iw = text_width(items[i], 3) + 40;
        int ix = (screen_width - iw) / 2;
        draw_rect(ix, y, iw, 50, COLOR_DARK);
        draw_text(ix+20, y+14, items[i], COLOR_WHITE, 3);
        y += 60;
    }
}

int menu_handle_touch(int x, int y, int pressed) {
    if (!menu_visible || pressed) return 0;
    int items_y = 100;
    const char *names[] = {"Calculator", "File Explorer", "Network", "Browser", "KUAL Apps", "Settings", "Exit"};
    for (int i = 0; i < 7; i++) {
        int iw = text_width(names[i], 3) + 40;
        int ix = (screen_width - iw) / 2;
        if (point_in_rect(x, y, ix, items_y, iw, 50)) {
            menu_visible = 0;
            if (i < 4 && registered_count > i) { app_open(&registered[i]); return 1; }
            if (i == 4) { system("killall kindlejap-bin 2>/dev/null"); return 1; }
            if (i == 6) { running = 0; return 1; }
            return 1;
        }
        items_y += 60;
    }
    return 1;
}

/* ========================= CALCULATOR APP ========================= */
static char calc_display[64] = "0";
static double calc_num1 = 0, calc_num2 = 0;
static int calc_op = 0;
static int calc_new_num = 1;

static void calc_init(void) { strcpy(calc_display, "0"); calc_num1 = 0; calc_num2 = 0; calc_op = 0; calc_new_num = 1; }

static void calc_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_rect(x, y, w, 60, COLOR_DARK);
    draw_text_right(y+20, calc_display, COLOR_WHITE, 3);
    const char *btns[] = {"C","(",")","/","7","8","9","*","4","5","6","-","1","2","3","+","0",".","=",""};
    int cols = 4, rows = 5;
    int bw = (w - 20) / cols, bh = (h - 80) / rows;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (idx >= 20) continue;
            int bx = x + 10 + c*bw, by = y + 70 + r*bh;
            draw_rect(bx+2, by+2, bw-4, bh-4, COLOR_LIGHT);
            int tw2 = text_width(btns[idx], 2);
            draw_text(bx + (bw-tw2)/2, by + bh/2 - 7, btns[idx], COLOR_BLACK, 2);
        }
}

static void calc_on_touch(int tx, int ty, int pressed) {
    if (pressed) return;
    int bw = (screen_width - 20) / 4, bh = (screen_height - TASKBAR_H - 80) / 5;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            int idx = r * 4 + c;
            if (idx >= 20) continue;
            int bx = 10 + c*bw, by = 70 + r*bh;
            if (!point_in_rect(tx, ty, bx, by, bw, bh)) continue;
            const char *labels[] = {"C","(",")","/","7","8","9","*","4","5","6","-","1","2","3","+","0",".","=",""};
            char ch = labels[idx][0];
            if (ch == 'C') { strcpy(calc_display, "0"); calc_num1 = 0; calc_op = 0; calc_new_num = 1; }
            else if (ch >= '0' && ch <= '9') {
                if (calc_new_num) { snprintf(calc_display, sizeof(calc_display), "%c", ch); calc_new_num = 0; }
                else { int l = strlen(calc_display); calc_display[l] = ch; calc_display[l+1] = 0; }
            }
            else if (ch == '.') { int l = strlen(calc_display); calc_display[l] = '.'; calc_display[l+1] = 0; calc_new_num = 0; }
            else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
                calc_num1 = atof(calc_display); calc_op = ch; calc_new_num = 1;
            }
            else if (ch == '=') {
                calc_num2 = atof(calc_display); double res = 0;
                if (calc_op == '+') res = calc_num1 + calc_num2;
                else if (calc_op == '-') res = calc_num1 - calc_num2;
                else if (calc_op == '*') res = calc_num1 * calc_num2;
                else if (calc_op == '/') res = calc_num2 != 0 ? calc_num1 / calc_num2 : 0;
                if (res == (int)res) snprintf(calc_display, sizeof(calc_display), "%d", (int)res);
                else snprintf(calc_display, sizeof(calc_display), "%.6g", res);
                calc_new_num = 1;
            }
            else if (ch == '(' || ch == ')') {}
            return;
        }
}

static App app_calc = {"Calculator", "CALC", calc_init, calc_draw, calc_on_touch, NULL, NULL};

/* ========================= FILE EXPLORER APP ========================= */
#define FE_MAX_ENTRIES 128
#define FE_NAME_LEN 64

typedef struct { char name[FE_NAME_LEN]; int is_dir; off_t size; } FEEntry;
static FEEntry fe_entries[FE_MAX_ENTRIES];
static int fe_count = 0;
static int fe_scroll = 0;
static char fe_path[256] = "/mnt/us";

static void fe_load_dir(const char *path) {
    DIR *d = opendir(path); if (!d) return;
    fe_count = 0; fe_scroll = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && fe_count < FE_MAX_ENTRIES) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1]=='.' && e->d_name[2]==0))) continue;
        strncpy(fe_entries[fe_count].name, e->d_name, FE_NAME_LEN-1);
        fe_entries[fe_count].name[FE_NAME_LEN-1] = 0;
        char full[512]; snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        fe_entries[fe_count].is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        fe_entries[fe_count].size = st.st_size;
        fe_count++;
    }
    closedir(d);
}

static void fe_init(void) { strcpy(fe_path, "/mnt/us"); fe_load_dir(fe_path); }

static void fe_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_rect(x, y, w, 50, COLOR_DARK);
    draw_text(x+10, y+16, fe_path, COLOR_WHITE, 2);
    int row_h = 40;
    int max_rows = (h - 60) / row_h;
    for (int i = 0; i < max_rows && i + fe_scroll < fe_count; i++) {
        int idx = i + fe_scroll;
        int ry = y + 56 + i * row_h;
        draw_rect(x+5, ry, w-10, row_h-2, (idx % 2) ? COLOR_LIGHT : COLOR_WHITE);
        if (fe_entries[idx].is_dir) draw_text(x+15, ry+12, "[DIR]", COLOR_DARK, 2);
        draw_text(x+60, ry+12, fe_entries[idx].name, COLOR_BLACK, 2);
        if (!fe_entries[idx].is_dir) {
            char sz[32]; snprintf(sz, sizeof(sz), "%ld", (long)fe_entries[idx].size);
            draw_text_right(ry+12, sz, COLOR_GRAY, 1);
        }
    }
}

static void fe_on_touch(int tx, int ty, int pressed) {
    if (pressed) return;
    int row_h = 40;
    int start_y = 56;
    if (ty < start_y) return;
    int row = (ty - start_y) / row_h + fe_scroll;
    if (row < 0 || row >= fe_count) return;
    if (fe_entries[row].is_dir) {
        if (strcmp(fe_entries[row].name, "..") == 0) {
            char *slash = strrchr(fe_path, '/');
            if (slash && slash > fe_path) { *slash = 0; if (fe_path[0] == 0) strcpy(fe_path, "/"); }
        } else {
            char newp[256]; snprintf(newp, sizeof(newp), "%s/%s", fe_path, fe_entries[row].name);
            strncpy(fe_path, newp, sizeof(fe_path)-1);
        }
        fe_load_dir(fe_path);
    }
}

static void fe_cleanup(void) { fe_count = 0; }

static App app_fe = {"Files", "FILES", fe_init, fe_draw, fe_on_touch, fe_cleanup, NULL};

/* ========================= NETWORK APP ========================= */
static char net_status[128] = "Scanning...";
static char net_ssid[64] = "";
static int net_connected = 0;
static char net_lines[20][128];
static int net_line_count = 0;
static int net_scroll = 0;

static void net_scan(void) {
    net_line_count = 0;
    system("iwlist wlan0 scan 2>/dev/null | grep ESSID > /tmp/kj_wifi.txt");
    FILE *f = fopen("/tmp/kj_wifi.txt", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && net_line_count < 20) {
            char *q = strchr(line, '"'); if (!q) continue;
            char *end = strchr(q+1, '"'); if (!end) continue;
            *end = 0;
            strncpy(net_lines[net_line_count], q+1, 127);
            net_line_count++;
        }
        fclose(f);
    }
    if (net_line_count == 0) { strcpy(net_lines[0], "No networks found"); net_line_count = 1; }
}

static void net_init(void) {
    strcpy(net_status, "Scanning..."); net_connected = 0; net_scroll = 0;
    net_scan();
    char cmd[256]; snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 status 2>/dev/null | grep ssid > /tmp/kj_conn.txt");
    system(cmd);
    FILE *f = fopen("/tmp/kj_conn.txt", "r");
    if (f) { char line[128]; if (fgets(line, sizeof(line), f)) { char *eq = strchr(line, '=');
        if (eq) { strncpy(net_ssid, eq+1, 63); char *nl = strchr(net_ssid, '\n'); if (nl) *nl = 0;
            net_connected = 1; snprintf(net_status, sizeof(net_status), "Connected: %s", net_ssid); }}
        fclose(f); }
}

static void net_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_rect(x, y, w, 50, COLOR_DARK);
    draw_text(x+10, y+16, "NETWORK", COLOR_WHITE, 3);
    draw_rect(x, y+54, w, 36, COLOR_LIGHT);
    draw_text(x+10, y+62, net_status, COLOR_BLACK, 2);
    int ry = y + 100;
    draw_rect(x+10, ry, w-20, 40, COLOR_DARK);
    draw_text(x+20, ry+12, "SCAN", COLOR_WHITE, 2);
    draw_rect(x+w/2+10, ry, w/2-20, 40, net_connected ? COLOR_DARK : COLOR_GRAY);
    draw_text(x+w/2+20, ry+12, "DISCONNECT", COLOR_WHITE, 2);
    ry += 50;
    int max_rows = (h - (ry-y) - 10) / 36;
    for (int i = 0; i < max_rows && i + net_scroll < net_line_count; i++) {
        int idx = i + net_scroll;
        int rry = ry + i * 36;
        draw_rect(x+5, rry, w-10, 34, (strcmp(net_lines[idx], net_ssid) == 0) ? COLOR_SELECT : COLOR_LIGHT);
        draw_text(x+15, rry+10, net_lines[idx], COLOR_BLACK, 2);
    }
}

static void net_on_touch(int tx, int ty, int pressed) {
    if (pressed) return;
    int ry = 100;
    if (point_in_rect(tx, ty, 10, ry, screen_width/2-20, 40)) { net_init(); return; }
    if (point_in_rect(tx, ty, screen_width/2+10, ry, screen_width/2-20, 40)) {
        if (net_connected) { system("wpa_cli -i wlan0 disconnect 2>/dev/null"); net_init(); }
        return;
    }
    int list_y = ry + 50;
    int row = (ty - list_y) / 36 + net_scroll;
    if (row >= 0 && row < net_line_count && ty >= list_y) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan1 connect \"%s\" 2>/dev/null || wpa_cli -i wlan0 connect \"%s\" 2>/dev/null", net_lines[row], net_lines[row]);
        system(cmd); net_init();
    }
}

static void net_cleanup(void) { net_line_count = 0; }

static App app_net = {"Network", "NET", net_init, net_draw, net_on_touch, net_cleanup, NULL};

/* ========================= BROWSER APP ========================= */
static char brw_url[256] = "";
static char brw_content[4096] = "";
static int brw_content_len = 0;
static int brw_scroll = 0;
static int brw_input_mode = 0;

static void brw_init(void) { brw_url[0] = 0; brw_content[0] = 0; brw_content_len = 0; brw_scroll = 0; brw_input_mode = 0; }

static void brw_got_url(const char *url) {
    brw_input_mode = 0; keyboard_hide();
    strncpy(brw_url, url, sizeof(brw_url)-1);
    snprintf(brw_content, sizeof(brw_content), "Loading %s...", brw_url);
    brw_content_len = strlen(brw_content);
    char cmd[1024]; snprintf(cmd, sizeof(cmd), "curl -sL --connect-timeout 5 --max-time 10 '%s' 2>/dev/null | head -c 4000 > /tmp/kj_page.txt", brw_url);
    system(cmd);
    FILE *f = fopen("/tmp/kj_page.txt", "r");
    if (f) { brw_content_len = fread(brw_content, 1, sizeof(brw_content)-1, f); brw_content[brw_content_len] = 0; fclose(f); }
    else { snprintf(brw_content, sizeof(brw_content), "Failed to load: %s", brw_url); brw_content_len = strlen(brw_content); }
}

static void brw_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_rect(x, y, w, 50, COLOR_DARK);
    draw_text(x+10, y+16, "BROWSER", COLOR_WHITE, 3);
    draw_rect(x, y+54, w, 36, COLOR_LIGHT);
    draw_text(x+10, y+64, brw_url[0] ? brw_url : "Tap GO to enter URL", COLOR_BLACK, 1);
    draw_rect(x+w-80, y+54, 80, 36, COLOR_DARK);
    draw_text(x+w-70, y+64, "GO", COLOR_WHITE, 2);
    int text_y = y + 100;
    int max_lines = (h - 110) / 14;
    int line = 0, ly = 0;
    for (int i = 0; i < brw_content_len && line - brw_scroll < max_lines; i++) {
        if (brw_content[i] == '\n') { line++; ly = 0; if (line < brw_scroll) continue; }
        if (line - brw_scroll >= 0) {
            int py = text_y + (line - brw_scroll) * 14;
            char ch[2] = {brw_content[i], 0};
            draw_text(x + 10 + ly, py, ch, COLOR_BLACK, 1);
        }
        ly += 6;
        if (ly > w - 20) { line++; ly = 0; if (line >= brw_scroll + max_lines) break; }
    }
}

static void brw_on_touch(int tx, int ty, int pressed) {
    if (pressed) return;
    if (point_in_rect(tx, ty, screen_width-80, 54, 80, 36)) {
        brw_input_mode = 1;
        keyboard_show(brw_got_url, brw_url);
        return;
    }
}

static void brw_cleanup(void) { brw_content[0] = 0; brw_content_len = 0; }

static App app_brw = {"Browser", "WEB", brw_init, brw_draw, brw_on_touch, brw_cleanup, NULL};

/* ========================= KUAL INTEGRATION ========================= */
static char kual_apps[32][128];
static int kual_count = 0;

static void kual_scan(void) {
    kual_count = 0;
    DIR *d = opendir("/mnt/us/extensions");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && kual_count < 32) {
        if (e->d_name[0] == '.') continue;
        char json[256]; snprintf(json, sizeof(json), "/mnt/us/extensions/%s/menu.json", e->d_name);
        if (access(json, F_OK) == 0) { strncpy(kual_apps[kual_count], e->d_name, 127); kual_count++; }
    }
    closedir(d);
}

/* ========================= COMMUNITY APPS ========================= */
static char community_names[32][64];
static char community_bins[32][256];
static int community_count = 0;

static void community_scan(void) {
    community_count = 0;
    mkdir(COMMUNITY_APPS_DIR, 0755);
    DIR *d = opendir(COMMUNITY_APPS_DIR); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && community_count < 32) {
        if (e->d_name[0] == '.') continue;
        char full[512]; snprintf(full, sizeof(full), "%s/%s", COMMUNITY_APPS_DIR, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            char bin[512]; snprintf(bin, sizeof(bin), "%s/app.bin", full);
            if (access(bin, X_OK) == 0) {
                strncpy(community_names[community_count], e->d_name, 63);
                strncpy(community_bins[community_count], bin, 255);
                community_count++;
            }
        }
    }
    closedir(d);
}

/* ========================= SIGNAL HANDLER ========================= */
void signal_handler(int sig) {
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT) {
        log_msg("FATAL SIGNAL received");
        restore_kindle_ui();
        _exit(1);
    }
    running = 0;
}

/* ========================= SPLASH SCREEN ========================= */
void show_splash(void) {
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
    draw_rect(0, screen_height/2 - 80, screen_width, 160, COLOR_DARK);
    draw_text_centered(screen_height/2 - 50, "KINDLEJAP", COLOR_WHITE, 4);
    draw_text_centered(screen_height/2, "v" KINDLEJAP_VERSION, COLOR_LIGHT, 2);
    refresh_screen();

    const char *steps[] = {"Initializing display", "Loading input devices", "Scanning apps", "Preparing launcher"};
    for (int i = 0; i < 4; i++) {
        int y = screen_height/2 + 80 + i * 30;
        draw_text_centered(y, steps[i], COLOR_DARK, 2);
        int pw = 300;
        draw_rect(screen_width/2 - pw/2, y - 10, pw, 4, COLOR_LIGHT);
        draw_rect(screen_width/2 - pw/2, y - 10, pw * (i+1) / 4, 4, COLOR_DARK);
        refresh_screen_partial();
        usleep(300000);
    }
    draw_text_centered(screen_height/2 + 220, "Ready!", COLOR_BLACK, 3);
    refresh_screen();
    usleep(500000);
}

/* ========================= MAIN ========================= */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler); signal(SIGBUS, signal_handler);
    signal(SIGABRT, signal_handler);

    log_msg("=== KindleJap " KINDLEJAP_VERSION " ===");
    if (acquire_lock() < 0) { log_msg("Already running"); exit(1); }

    log_msg("init_framebuffer"); init_framebuffer();
    show_splash();
    log_msg("init_input"); init_input();

    app_register(&app_calc);
    app_register(&app_fe);
    app_register(&app_net);
    app_register(&app_brw);

    kual_scan(); community_scan();
    log_msg("setup done");

    active_app_idx = -1;
    open_count = 0;

    while (running) {
        draw_rect(0, 0, screen_width, screen_height - TASKBAR_H, COLOR_WHITE);

        if (menu_visible) {
            menu_draw();
        } else if (active_app_idx >= 0 && active_app_idx < open_count) {
            int app_h = screen_height - TASKBAR_H;
            if (keyboard_visible) app_h -= KEYBOARD_H;
            open_apps[active_app_idx]->draw(0, 0, screen_width, app_h);
            keyboard_draw();
        } else {
            draw_rect(0, 0, screen_width, screen_height/3, COLOR_DARK);
            draw_text_centered(screen_height/6 - 15, "KINDLEJAP", COLOR_WHITE, 4);
            draw_text_centered(screen_height/6 + 20, "v" KINDLEJAP_VERSION, COLOR_LIGHT, 2);
            draw_text_centered(screen_height/3 + 40, "Tap MENU to open apps", COLOR_DARK, 2);
            draw_text_centered(screen_height/3 + 80, "Hold menu tab to close app", COLOR_GRAY, 1);
        }

        taskbar_draw();
        refresh_screen_partial();

        if (input_fd >= 0) {
            struct input_event ev;
            static int touch_x = 0, touch_y = 0, touching = 0;
            while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) touch_x = ev.value;
                    if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) touch_y = ev.value;
                }
                if (ev.type == EV_KEY && ev.code == BTN_TOUCH) touching = ev.value;
                if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    if (touching) {
                        if (keyboard_visible) {
                            if (touch_y >= kb_start_y()) keyboard_handle_touch(touch_x, touch_y);
                            else if (active_app_idx >= 0) open_apps[active_app_idx]->on_touch(touch_x, touch_y, 1);
                        } else if (touch_y >= screen_height - TASKBAR_H) {
                            taskbar_handle_touch(touch_x, touch_y, 1);
                        } else if (menu_visible) {
                            menu_handle_touch(touch_x, touch_y, 1);
                        } else if (active_app_idx >= 0) {
                            open_apps[active_app_idx]->on_touch(touch_x, touch_y, 1);
                        }
                    } else {
                        if (keyboard_visible && touch_y >= kb_start_y()) {
                        } else if (touch_y >= screen_height - TASKBAR_H) {
                            taskbar_handle_touch(touch_x, touch_y, 0);
                        } else if (menu_visible) {
                            menu_handle_touch(touch_x, touch_y, 0);
                        } else if (active_app_idx >= 0) {
                            open_apps[active_app_idx]->on_touch(touch_x, touch_y, 0);
                        }
                    }
                }
            }
        }
        usleep(16000);
    }

    log_msg("shutting down");
    for (int i = open_count - 1; i >= 0; i--) app_close(i);
    restore_kindle_ui();
    release_lock();
    if (fb_mem) munmap(fb_mem, finfo.smem_len);
    if (fb_fd >= 0) close(fb_fd);
    if (input_fd >= 0) close(input_fd);
    log_msg("exit");
    return 0;
}
