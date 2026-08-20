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
#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>

#define KINDLEJAP_VERSION "3.1.0"
#define GITHUB_API_URL "https://api.github.com/repos/victorbillyph/kindlejap/releases/latest"
#define UPDATE_SCRIPT "/mnt/us/extensions/kindlejap/bin/update.sh"
#define LOCKFILE "/tmp/kindlejap.lock"
#define LOGFILE "/mnt/us/kindlejap.log"
#define DATA_DIR "/mnt/us/extensions/kindlejap/data"
#define SETTINGS_FILE "/mnt/us/extensions/kindlejap/data/settings.cfg"
#define APPSTATE_FILE "/mnt/us/extensions/kindlejap/data/appstate.cfg"
#define SETUP_DONE_FILE "/mnt/us/extensions/kindlejap/data/setup_done"
#define COMMUNITY_APPS_DIR "/mnt/us/kindlejap_apps"
#define TOPBAR_H 40
#define KEYBOARD_H 360
#define FONT_W 8
#define FONT_H 13
#define CORNER_R 8
#define COLOR_WHITE 0xFF
#define COLOR_BLACK 0x00
#define COLOR_DARK 0x30
#define COLOR_MID 0xA0
#define COLOR_LIGHT 0xE8
#define COLOR_LIGHTER 0xD0

static int log_fd = -1;
void log_msg(const char *msg) {
    if (log_fd < 0) log_fd = open(LOGFILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd >= 0) { write(log_fd, msg, strlen(msg)); write(log_fd, "\n", 1); fsync(log_fd); }
}

typedef struct {
    char last_path[256];
    char browser_url[256];
    int file_scroll;
} AppSettings;

static AppSettings settings;

static void data_init(void) {
    struct stat st;
    if (stat(DATA_DIR, &st) != 0) mkdir(DATA_DIR, 0777);
    memset(&settings, 0, sizeof(settings));
    strcpy(settings.last_path, "/mnt/us");
    strcpy(settings.browser_url, "https://");
}

static void data_save_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "last_path=%s\n", settings.last_path);
    fprintf(f, "browser_url=%s\n", settings.browser_url);
    fprintf(f, "file_scroll=%d\n", settings.file_scroll);
    fclose(f);
    log_msg("Settings saved");
}

static void data_load_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; char *key = line, *val = eq+1;
        if (strcmp(key, "last_path") == 0) strncpy(settings.last_path, val, sizeof(settings.last_path)-1);
        else if (strcmp(key, "browser_url") == 0) strncpy(settings.browser_url, val, sizeof(settings.browser_url)-1);
        else if (strcmp(key, "file_scroll") == 0) settings.file_scroll = atoi(val);
    }
    fclose(f);
    log_msg("Settings loaded");
}

static void data_save_appstate(int app_idx) {
    FILE *f = fopen(APPSTATE_FILE, "w");
    if (!f) return;
    fprintf(f, "active_app=%d\n", app_idx);
    fclose(f);
}

static int data_load_appstate(void) {
    int idx = -1;
    FILE *f = fopen(APPSTATE_FILE, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; char *key = line, *val = eq+1;
        if (strcmp(key, "active_app") == 0) idx = atoi(val);
    }
    fclose(f);
    return idx;
}

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
int fb_fd = -1;
unsigned char *fb_mem = NULL;
int screen_width, screen_height, bytes_per_pixel;
int input_fd = -1;
int power_btn_fd = -1;
volatile int running = 1;
int lock_fd = -1;
static int dirty = 1;
static int downbar_visible = 0;
static pid_t kindle_gui_pid = -1;

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
        d.temp = 4097;
        if (ioctl(fb_fd, MXCFB_SEND_UPDATE_K51, &d) == 0) {
            mxcfb_mode = 1;
            struct mxcfb_update_marker_data md = {refresh_marker, 0};
            ioctl(fb_fd, MXCFB_WAIT_COMPLETE_CARTA, &md);
            return 0;
        }
        struct mxcfb_update_data_zelda dz;
        memset(&dz, 0, sizeof(dz));
        dz.update_region.width = screen_width; dz.update_region.height = screen_height;
        dz.waveform_mode = wfm; dz.update_mode = full ? UPD_FULL : UPD_PARTIAL;
        dz.update_marker = refresh_marker; dz.temp = 4096; dz.flags = 0;
        dz.hist_bw_waveform_mode = 1;
        dz.hist_gray_waveform_mode = full ? WFM_GC16 : WFM_GC16_FAST;
        if (ioctl(fb_fd, MXCFB_SEND_UPDATE_ZELDA, &dz) == 0) {
            mxcfb_mode = 2;
            struct mxcfb_update_marker_data md = {refresh_marker, 0};
            ioctl(fb_fd, MXCFB_WAIT_COMPLETE_CARTA, &md);
            return 0;
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
    if (kindle_gui_pid > 0) {
        char cmd[64]; snprintf(cmd, sizeof(cmd), "kill -CONT %d 2>/dev/null", kindle_gui_pid);
        system(cmd);
        log_msg("Resumed Kindle UI");
    } else {
        system("initctl start lab126_gui 2>/dev/null");
    }
    system("initctl start otaupd 2>/dev/null");
    system("initctl start phd 2>/dev/null");
    system("initctl start tmd 2>/dev/null");
    system("initctl start todo 2>/dev/null");
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
    char buf[128];
    snprintf(buf, sizeof(buf), "FB: %dx%d %dbpp", screen_width, screen_height, vinfo.bits_per_pixel);
    log_msg(buf);
}

void init_input(void) {
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e; int best = -1, best_s = -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[64]; snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        char name[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        int s = 0;
        if (strstr(name, "touch") || strstr(name, "Touch")) s += 10;
        if (strstr(name, "multi") || strstr(name, "Multi")) s += 5;
        if (s > best_s) { if (best >= 0) close(best); best = fd; best_s = s; }
        else close(fd);
    }
    closedir(d);
    input_fd = best;
}

static void init_power_button(void) {
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[64]; snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        char name[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strstr(name, "Power") || strstr(name, "power") || strstr(name, "max77696")) {
            power_btn_fd = fd; log_msg("Power button found"); break;
        }
        close(fd);
    }
    closedir(d);
}

static int read_battery(void) {
    DIR *d = opendir("/sys/class/power_supply"); if (!d) return -1;
    struct dirent *e; int pct = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[128]; snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) { if (fscanf(f, "%d", &pct) == 1) { fclose(f); closedir(d); return pct; } fclose(f); }
    }
    closedir(d);
    return -1;
}

static int read_wifi(void) {
    char buf[8] = {0};
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) f = fopen("/sys/class/net/eth0/operstate", "r");
    if (!f) return 0;
    int r = fread(buf, 1, 7, f); fclose(f);
    (void)r;
    return (buf[0] == 'u') ? 1 : 0;
}

void draw_pixel(int x, int y, unsigned char c) {
    if (x<0||x>=screen_width||y<0||y>=screen_height) return;
    int off = y*finfo.line_length+x*bytes_per_pixel;
    if (off>=0&&off<(int)finfo.smem_len) { fb_mem[off]=c; if(bytes_per_pixel>1) fb_mem[off+1]=c; }
}

void draw_rect(int x, int y, int w, int h, unsigned char c) {
    for (int j=y; j<y+h&&j<screen_height; j++)
        for (int i=x; i<x+w&&i<screen_width; i++) draw_pixel(i, j, c);
}

void draw_rounded_rect(int x, int y, int w, int h, int r, unsigned char c) {
    if (r>w/2) r=w/2; if (r>h/2) r=h/2;
    for (int j=y; j<y+h&&j<screen_height; j++) {
        for (int i=x; i<x+w&&i<screen_width; i++) {
            int dx=0, dy=0;
            if (i<x+r&&j<y+r) { dx=x+r-i; dy=y+r-j; }
            else if (i>=x+w-r&&j<y+r) { dx=i-(x+w-r-1); dy=y+r-j; }
            else if (i<x+r&&j>=y+h-r) { dx=x+r-i; dy=j-(y+h-r-1); }
            else if (i>=x+w-r&&j>=y+h-r) { dx=i-(x+w-r-1); dy=j-(y+h-r-1); }
            else { draw_pixel(i, j, c); continue; }
            if (dx*dx+dy*dy<=r*r) draw_pixel(i, j, c);
        }
    }
}

void draw_circle(int cx, int cy, int r, unsigned char c) {
    for (int j=cy-r; j<=cy+r; j++)
        for (int i=cx-r; i<=cx+r; i++)
            if ((i-cx)*(i-cx)+(j-cy)*(j-cy)<=r*r) draw_pixel(i, j, c);
}

void draw_line(int x0, int y0, int x1, int y1, unsigned char c) {
    int dx=abs(x1-x0), dy=-abs(y1-y0);
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx+dy;
    while (1) {
        draw_pixel(x0, y0, c);
        if (x0==x1&&y0==y1) break;
        int e2=2*err;
        if (e2>=dy) { err+=dy; x0+=sx; }
        if (e2<=dx) { err+=dx; y0+=sy; }
    }
}

void draw_bmp(const char *path, int ox, int oy) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return; }
    if (hdr[0]!='B'||hdr[1]!='M') { fclose(f); return; }
    int w=*(int*)&hdr[18], h=*(int*)&hdr[22];
    int bpp=*(short*)&hdr[28]; if (bpp!=24) { fclose(f); return; }
    int rs=((w*3+3)&~3);
    unsigned char *row = malloc(rs);
    if (!row) { fclose(f); return; }
    for (int j=0; j<abs(h); j++) {
        fread(row, 1, rs, f);
        int sy = h>0 ? (oy+h-1-j) : (oy+j);
        for (int i=0; i<w; i++) {
            int b=row[i*3], g=row[i*3+1], rv=row[i*3+2];
            draw_pixel(ox+i, sy, (unsigned char)((rv*77+g*150+b*29)>>8));
        }
    }
    free(row); fclose(f);
}

void draw_pgm(const char *path, int ox, int oy) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char m[3]={0}; fgets(m, 3, f);
    if (m[0]!='P'||m[1]!='5') { fclose(f); return; }
    int w=0, h=0, mv=255;
    while (1) { int c=fgetc(f); if (c=='#') { while (fgetc(f)!='\n'); }
        else if (c>='0'&&c<='9') { ungetc(c, f); fscanf(f, "%d", &w); break; } }
    fscanf(f, "%d", &h); fscanf(f, "%d", &mv); fgetc(f);
    for (int j=0; j<h; j++)
        for (int i=0; i<w; i++) {
            unsigned char px = fgetc(f);
            if (mv!=255) px = (unsigned char)((int)px*255/mv);
            draw_pixel(ox+i, oy+j, px);
        }
    fclose(f);
}

static const unsigned char font8x13[95][13] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00},
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00},
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18},
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00},
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00},
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x18,0x18,0x18,0xFE,0x18,0x18,0x18,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00},
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00},
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00},
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00},
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00},
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00},
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00},
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00},
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00},
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00},
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00},
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00},
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00},
    {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00},
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00},
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C},
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00},
    {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00},
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0x00,0x00,0x00,0x40,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00},
    {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00},
    {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C},
    {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00},
    {0x00,0x00,0x18,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0x06,0x00,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x6C},
    {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00},
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x7C,0x60,0x60},
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C},
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
    {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x7C},
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00},
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00},
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00},
    {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

void draw_char(int x, int y, char ch, unsigned char color, int scale) {
    int idx = ch - 32; if (idx<0||idx>=95) return;
    for (int row=0; row<FONT_H; row++) {
        unsigned char bits = font8x13[idx][row];
        for (int col=0; col<FONT_W; col++)
            if (bits & (0x80>>col))
                for (int sy=0; sy<scale; sy++)
                    for (int sx=0; sx<scale; sx++)
                        draw_pixel(x+col*scale+sx, y+row*scale+sy, color);
    }
}

void draw_text(int x, int y, const char *t, unsigned char c, int s) {
    while (*t) { draw_char(x, y, *t, c, s); x += FONT_W*s; t++; }
}

void draw_text_centered_in(int x, int y, int w, const char *t, unsigned char c, int s) {
    int tw = (int)strlen(t) * FONT_W * s;
    draw_text(x + (w-tw)/2, y, t, c, s);
}

void draw_text_right(int xl, int y, const char *t, unsigned char c, int s) {
    int w = (int)strlen(t) * FONT_W * s;
    draw_text(xl-w, y, t, c, s);
}

int text_width(const char *t, int s) { return (int)strlen(t) * FONT_W * s; }

int point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px>=x && px<x+w && py>=y && py<y+h;
}

static int keyboard_visible = 0;
static int keyboard_mode = 0;
static int keyboard_cursor = 0;
static char keyboard_buf[128] = "";

static const char *kb_rows_lower[3] = {
    "qwertyuiop", "asdfghjkl", "zxcvbnm"
};
static const char *kb_rows_upper[3] = {
    "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
};
static const char *kb_rows_sym[3] = {
    ".,!?@#$%&*", "-+=/\\|~()", ";:'\"<>^"
};

void keyboard_draw(void) {
    int ky = screen_height - KEYBOARD_H;
    draw_rounded_rect(0, ky, screen_width, KEYBOARD_H, CORNER_R, COLOR_LIGHTER);
    draw_rounded_rect(10, ky+10, screen_width-20, 38, 8, COLOR_WHITE);
    if (keyboard_cursor > 0) {
        draw_text(18, ky+18, keyboard_buf, COLOR_BLACK, 2);
        int tw = text_width(keyboard_buf, 2);
        if ((time(NULL)*2)%2==0) draw_rect(18+tw, ky+14, 2, 34, COLOR_BLACK);
    }
    const char **rows = (keyboard_mode == 1) ? kb_rows_upper : (keyboard_mode == 2) ? kb_rows_sym : kb_rows_lower;
    int bw = (screen_width-60) / 10;
    int sy = ky + 58;
    const char *mode_labels[] = {"abc", "ABC", "#123"};
    unsigned char mode_colors[] = {COLOR_MID, COLOR_DARK, COLOR_LIGHT};
    draw_rounded_rect(10, sy, 80, 48, 8, mode_colors[keyboard_mode]);
    draw_text_centered_in(10, sy+14, 80, mode_labels[keyboard_mode], COLOR_WHITE, 2);
    draw_rounded_rect(screen_width-90, sy, 80, 48, 8, COLOR_DARK);
    draw_text_centered_in(screen_width-90, sy+14, 80, "Enter", COLOR_WHITE, 2);
    int bkx = screen_width/2 - 55;
    draw_rounded_rect(bkx, sy, 110, 48, 8, COLOR_DARK);
    draw_text_centered_in(bkx, sy+14, 110, "Bksp", COLOR_WHITE, 2);
    for (int row=0; row<3; row++) {
        int len = strlen(rows[row]);
        int off = (10-len) * bw / 2;
        for (int i=0; i<len; i++) {
            int bx = 30 + off + i*bw;
            int by = ky + 116 + row*58;
            unsigned char bg = (keyboard_mode == 2) ? COLOR_LIGHT : COLOR_WHITE;
            draw_rounded_rect(bx, by, bw-4, 52, 8, bg);
            char label[2] = {rows[row][i], 0};
            draw_text_centered_in(bx, by+14, bw-4, label, COLOR_BLACK, 2);
        }
    }
    int spx = 30, spw = screen_width-60;
    int spy = ky + 116 + 3*58;
    draw_rounded_rect(spx, spy, spw, 52, 8, COLOR_WHITE);
    draw_text_centered_in(spx, spy+14, spw, "SPACE", COLOR_BLACK, 2);
}

void keyboard_handle_touch(int tx, int ty) {
    int ky = screen_height - KEYBOARD_H;
    int sy = ky + 58;
    if (point_in_rect(tx, ty, 10, sy, 80, 48)) {
        keyboard_mode = (keyboard_mode + 1) % 3;
        return;
    }
    if (point_in_rect(tx, ty, screen_width-90, sy, 80, 48)) {
        keyboard_visible = 0; return;
    }
    int bkx = screen_width/2 - 55;
    if (point_in_rect(tx, ty, bkx, sy, 110, 48)) {
        if (keyboard_cursor > 0) { keyboard_cursor--; keyboard_buf[keyboard_cursor]=0; }
        return;
    }
    const char **rows = (keyboard_mode == 1) ? kb_rows_upper : (keyboard_mode == 2) ? kb_rows_sym : kb_rows_lower;
    int bw = (screen_width-60)/10;
    for (int row=0; row<3; row++) {
        int len = strlen(rows[row]);
        int off = (10-len)*bw/2;
        for (int i=0; i<len; i++) {
            int bx = 30+off+i*bw;
            int by = ky+116+row*58;
            if (point_in_rect(tx, ty, bx, by, bw-4, 52)) {
                char c = rows[row][i];
                if (keyboard_cursor < (int)sizeof(keyboard_buf)-1) {
                    keyboard_buf[keyboard_cursor++] = c;
                    keyboard_buf[keyboard_cursor] = 0;
                }
                return;
            }
        }
    }
    int spy = ky + 116 + 3*58;
    if (point_in_rect(tx, ty, 30, spy, screen_width-60, 52)) {
        if (keyboard_cursor < (int)sizeof(keyboard_buf)-1) {
            keyboard_buf[keyboard_cursor++] = ' ';
            keyboard_buf[keyboard_cursor] = 0;
        }
        return;
    }
}

typedef struct {
    const char *name;
    void (*init)(void);
    void (*draw)(int x, int y, int w, int h);
    void (*on_touch)(int x, int y, int released);
    void (*cleanup)(void);
} App;

#define MAX_REGISTERED_APPS 32
#define MAX_OPEN_APPS 8

static App registered[MAX_REGISTERED_APPS];
static int registered_count = 0;
static App *open_apps[MAX_OPEN_APPS];
static int open_count = 0;
static int active_app_idx = -1;

static void calc_draw(int x, int y, int w, int h);
static void calc_handle(int x, int y, int released);
static void file_draw(int x, int y, int w, int h);
static void file_handle(int x, int y, int released);
static void net_init(void);
static void net_draw(int x, int y, int w, int h);
static void net_handle(int x, int y, int released);
static void browser_draw(int x, int y, int w, int h);
static void browser_handle(int x, int y, int released);
static void pkg_draw(int x, int y, int w, int h);
static void pkg_handle(int x, int y, int released);
static void check_update(void);

static App calc_app = {"Calculator", NULL, calc_draw, calc_handle, NULL};
static App file_app = {"Files", NULL, file_draw, file_handle, NULL};
static App net_app = {"Network", net_init, net_draw, net_handle, NULL};
static App browser_app = {"Browser", NULL, browser_draw, browser_handle, NULL};
static App pkg_app = {"Package Manager", NULL, pkg_draw, pkg_handle, NULL};

void app_register(App *app) {
    if (registered_count < MAX_REGISTERED_APPS) registered[registered_count++] = *app;
}

void app_open(App *app) {
    for (int i=0; i<open_count; i++)
        if (open_apps[i] == app) { active_app_idx = i; return; }
    if (open_count >= MAX_OPEN_APPS) return;
    open_apps[open_count] = app;
    active_app_idx = open_count;
    open_count++;
    if (app->init) app->init();
}

void app_close_idx(int idx) {
    if (idx<0||idx>=open_count) return;
    if (open_apps[idx]->cleanup) open_apps[idx]->cleanup();
    for (int i=idx; i<open_count-1; i++) open_apps[i] = open_apps[i+1];
    open_count--;
    if (active_app_idx >= open_count) active_app_idx = open_count-1;
}

void app_close_active(void) {
    if (active_app_idx >= 0 && active_app_idx < open_count) app_close_idx(active_app_idx);
}

static int menu_visible = 0;

#define MAX_NOTIFICATIONS 16
#define NOTIF_SIDEBAR_W 400

typedef struct {
    char title[128];
    char message[256];
    char action_label[64];
    int has_action;
    int active;
} Notification;

static Notification notifications[MAX_NOTIFICATIONS];
static int notif_count = 0;
static int notif_sidebar_visible = 0;
static int notif_scroll = 0;

static int notif_get_active(void) {
    int c = 0;
    for (int i = 0; i < notif_count; i++)
        if (notifications[i].active) c++;
    return c;
}

static void notif_add(const char *title, const char *message, const char *action_label) {
    if (notif_count >= MAX_NOTIFICATIONS) {
        for (int i = 0; i < notif_count - 1; i++)
            notifications[i] = notifications[i+1];
        notif_count--;
    }
    memset(&notifications[notif_count], 0, sizeof(Notification));
    strncpy(notifications[notif_count].title, title, 127);
    strncpy(notifications[notif_count].message, message, 255);
    if (action_label && strlen(action_label) > 0) {
        strncpy(notifications[notif_count].action_label, action_label, 63);
        notifications[notif_count].has_action = 1;
    }
    notifications[notif_count].active = 1;
    notif_count++;
    dirty = 1;
}

static void notif_dismiss(int idx) {
    if (idx >= 0 && idx < notif_count) {
        notifications[idx].active = 0;
        dirty = 1;
    }
}

static void notif_clear_all(void) {
    for (int i = 0; i < notif_count; i++)
        notifications[i].active = 0;
    dirty = 1;
}

static void topbar_draw(void) {
    draw_rect(0, 0, screen_width, TOPBAR_H, COLOR_DARK);
    int wifi = read_wifi();
    draw_text(16, 10, wifi ? "WiFi" : "---", COLOR_WHITE, 2);
    int bat = read_battery();
    if (bat >= 0) {
        char bstr[16]; snprintf(bstr, sizeof(bstr), "%d%%", bat);
        int bw = text_width(bstr, 2);
        draw_text(screen_width - bw - 16, 10, bstr, COLOR_WHITE, 2);
    }
    int nc = notif_get_active();
    int nx = screen_width - 180;
    draw_text(nx, 10, "[!]", COLOR_WHITE, 2);
    if (nc > 0) {
        char nstr[8]; snprintf(nstr, sizeof(nstr), "%d", nc);
        draw_rounded_rect(nx + 32, 6, text_width(nstr, 2) + 12, 24, 12, COLOR_WHITE);
        draw_text(nx + 38, 10, nstr, COLOR_DARK, 2);
    }
}

static void downbar_draw(void) {
    if (!downbar_visible) return;
    draw_rect(0, TOPBAR_H, screen_width, 60, COLOR_LIGHT);
    draw_rounded_rect(10, TOPBAR_H+10, 100, 40, 8, COLOR_WHITE);
    draw_text_centered_in(10, TOPBAR_H+18, 100, "Menu", COLOR_BLACK, 2);
    draw_rounded_rect(screen_width/2-50, TOPBAR_H+10, 100, 40, 8, COLOR_WHITE);
    draw_text_centered_in(screen_width/2-50, TOPBAR_H+18, 100, "Sleep", COLOR_BLACK, 2);
    draw_rounded_rect(screen_width-110, TOPBAR_H+10, 100, 40, 8, COLOR_WHITE);
    draw_text_centered_in(screen_width-110, TOPBAR_H+18, 100, "Exit", COLOR_BLACK, 2);
}

static void menu_draw(void) {
    if (!menu_visible) return;
    int mw = 280, mh = 392;
    int mx = (screen_width - mw) / 2, my = TOPBAR_H + 80;
    draw_rounded_rect(mx, my, mw, mh, CORNER_R, COLOR_WHITE);
    const char *items[] = {"Calculator", "Files", "Network", "Browser", "Package Manager", "Check Update", "Close"};
    for (int i=0; i<7; i++) {
        int iy = my + 10 + i*46;
        draw_rounded_rect(mx+10, iy, mw-20, 40, 8, COLOR_LIGHT);
        draw_text_centered_in(mx+10, iy+10, mw-20, items[i], COLOR_BLACK, 2);
    }
}

static void downbar_handle_touch(int tx, int ty) {
    if (point_in_rect(tx, ty, 10, TOPBAR_H+10, 100, 40)) {
        menu_visible = 1; downbar_visible = 0; return;
    }
    if (point_in_rect(tx, ty, screen_width/2-50, TOPBAR_H+10, 100, 40)) {
        downbar_visible = 0;
        system("lipc-set-prop com.lab126.powerd sleep 1 2>/dev/null");
        return;
    }
    if (point_in_rect(tx, ty, screen_width-110, TOPBAR_H+10, 100, 40)) {
        running = 0; downbar_visible = 0; return;
    }
}

static void menu_handle_touch(int tx, int ty) {
    int mw=280, mh=392;
    int mx=(screen_width-mw)/2, my=TOPBAR_H+80;
    if (!point_in_rect(tx, ty, mx, my, mw, mh)) { menu_visible=0; return; }
    App *apps[] = {&calc_app, &file_app, &net_app, &browser_app};
    for (int i=0; i<4; i++) {
        int iy = my+10+i*46;
        if (point_in_rect(tx, ty, mx+10, iy, mw-20, 40)) {
            app_open(apps[i]); menu_visible=0; return;
        }
    }
    int iy4 = my+10+4*46;
    if (point_in_rect(tx, ty, mx+10, iy4, mw-20, 40)) {
        app_open(&pkg_app); menu_visible=0; return;
    }
    int iy5 = my+10+5*46;
    if (point_in_rect(tx, ty, mx+10, iy5, mw-20, 40)) {
        menu_visible=0;
        check_update();
        return;
    }
    int iy6 = my+10+6*46;
    if (point_in_rect(tx, ty, mx+10, iy6, mw-20, 40)) {
        menu_visible=0;
    }
}

static void notif_sidebar_draw(void) {
    if (!notif_sidebar_visible) return;
    int sx = screen_width - NOTIF_SIDEBAR_W;
    draw_rect(sx, 0, NOTIF_SIDEBAR_W, screen_height, COLOR_LIGHTER);
    draw_rounded_rect(sx+10, 10, NOTIF_SIDEBAR_W-20, 40, 8, COLOR_WHITE);
    draw_text_centered_in(sx+10, 18, NOTIF_SIDEBAR_W-20, "Notifications", COLOR_BLACK, 2);
    int nc = notif_get_active();
    if (nc > 0) {
        draw_rounded_rect(sx+NOTIF_SIDEBAR_W-80, 10, 70, 40, 8, COLOR_MID);
        draw_text_centered_in(sx+NOTIF_SIDEBAR_W-80, 18, 70, "Clear", COLOR_WHITE, 2);
    }
    int iy = 60;
    int shown = 0;
    for (int i = notif_count - 1; i >= 0 && iy < screen_height - 10; i--) {
        if (!notifications[i].active) continue;
        if (shown < notif_scroll) { shown++; continue; }
        shown++;
        int card_h = notifications[i].has_action ? 100 : 70;
        draw_rounded_rect(sx+10, iy, NOTIF_SIDEBAR_W-20, card_h, 8, COLOR_WHITE);
        draw_text(sx+18, iy+8, notifications[i].title, COLOR_BLACK, 2);
        draw_text(sx+18, iy+30, notifications[i].message, COLOR_DARK, 1);
        if (notifications[i].has_action) {
            draw_rounded_rect(sx+10, iy+55, NOTIF_SIDEBAR_W-40, 32, 8, COLOR_MID);
            draw_text_centered_in(sx+10, iy+61, NOTIF_SIDEBAR_W-40, notifications[i].action_label, COLOR_WHITE, 2);
        }
        iy += card_h + 8;
    }
    if (nc == 0) {
        draw_text_centered_in(sx, screen_height/2, NOTIF_SIDEBAR_W, "No notifications", COLOR_MID, 2);
    }
}

static void notif_sidebar_handle(int tx, int ty) {
    int sx = screen_width - NOTIF_SIDEBAR_W;
    if (!point_in_rect(tx, ty, sx, 0, NOTIF_SIDEBAR_W, screen_height)) {
        notif_sidebar_visible = 0; return;
    }
    if (notif_get_active() > 0 && point_in_rect(tx, ty, sx+NOTIF_SIDEBAR_W-80, 10, 70, 40)) {
        notif_clear_all(); return;
    }
    int iy = 60;
    int shown = 0;
    for (int i = notif_count - 1; i >= 0; i--) {
        if (!notifications[i].active) continue;
        if (shown < notif_scroll) { shown++; continue; }
        shown++;
        int card_h = notifications[i].has_action ? 100 : 70;
        if (notifications[i].has_action && point_in_rect(tx, ty, sx+10, iy+55, NOTIF_SIDEBAR_W-40, 32)) {
            notif_dismiss(i);
            dirty = 1;
            return;
        }
        if (point_in_rect(tx, ty, sx+10, iy, NOTIF_SIDEBAR_W-20, card_h)) {
            notif_dismiss(i);
            dirty = 1;
            return;
        }
        iy += card_h + 8;
    }
}

static int update_state = 0;
static char update_version[32] = "";
static char update_url[256] = "";

static void check_update(void) {
    log_msg("Checking for update...");
    update_state = 1;
    system("curl -s " GITHUB_API_URL " > /tmp/kindlejap_update.json 2>/dev/null");
    FILE *f = fopen("/tmp/kindlejap_update.json", "r");
    if (!f) { update_state = 0; return; }
    char buf[2048]; int n = fread(buf, 1, sizeof(buf)-1, f);
    buf[n] = 0; fclose(f);
    char *tag = strstr(buf, "\"tag_name\"");
    if (tag) {
        tag = strchr(tag, ':');
        if (tag) {
            tag++;
            while (*tag==' '||*tag=='"') tag++;
            char *end = strchr(tag, '"');
            if (end) { int l=end-tag; if(l<31) { strncpy(update_version, tag, l); update_version[l]=0; } }
        }
    }
    char *dl = strstr(buf, "\"browser_download_url\"");
    if (dl) {
        dl = strchr(dl, ':');
        if (dl) {
            dl++;
            while (*dl==' '||*dl=='"') dl++;
            char *end = strchr(dl, '"');
            if (end) { int l=end-dl; if(l<255) { strncpy(update_url, dl, l); update_url[l]=0; } }
        }
    }
    if (strlen(update_version) > 0 && strcmp(update_version, KINDLEJAP_VERSION) != 0) {
        char msg[128]; snprintf(msg, sizeof(msg), "Update available: %s", update_version);
        log_msg(msg);
    } else {
        log_msg("No update available");
        update_state = 0;
    }
}

static void update_draw(void) {
    if (update_state != 1) return;
    int dw=400, dh=200;
    int dx=(screen_width-dw)/2, dy=(screen_height-dh)/2;
    draw_rounded_rect(dx, dy, dw, dh, CORNER_R, COLOR_WHITE);
    draw_text(dx+20, dy+20, "Update Available", COLOR_BLACK, 3);
    draw_text(dx+20, dy+70, update_version, COLOR_DARK, 2);
    draw_rounded_rect(dx+20, dy+120, 170, 50, 8, COLOR_MID);
    draw_text_centered_in(dx+20, dy+132, 170, "Install", COLOR_WHITE, 2);
    draw_rounded_rect(dx+210, dy+120, 170, 50, 8, COLOR_LIGHT);
    draw_text_centered_in(dx+210, dy+132, 170, "Cancel", COLOR_DARK, 2);
}

static void update_handle(int tx, int ty, int released) {
    if (!released || update_state!=1) return;
    int dw=400, dh=200;
    int dx=(screen_width-dw)/2, dy=(screen_height-dh)/2;
    if (point_in_rect(tx, ty, dx+20, dy+120, 170, 50)) {
        update_state = 2;
        char cmd[512]; snprintf(cmd, sizeof(cmd), "%s \"%s\" &", UPDATE_SCRIPT, KINDLEJAP_VERSION);
        system(cmd);
        running = 0;
    }
    if (point_in_rect(tx, ty, dx+210, dy+120, 170, 50)) {
        update_state = 0;
    }
}

static double calc_val = 0;
static double calc_mem = 0;
static int calc_op = 0;
static int calc_new = 1;
static char calc_display[64] = "0";

static void calc_draw(int x, int y, int w, int h) {
    draw_rounded_rect(x+10, y+10, w-20, 50, 8, COLOR_WHITE);
    draw_text_right(w-20, y+20, calc_display, COLOR_BLACK, 3);
    const char *keys[] = {"7","8","9","+","4","5","6","-","1","2","3","*","C","0","=","/"};
    int bw=(w-40)/4, bh=60;
    for (int r=0; r<4; r++)
        for (int c=0; c<4; c++) {
            int kx=x+10+c*bw, ky=y+80+r*bh;
            unsigned char bg = (r*4+c==12) ? COLOR_MID : COLOR_LIGHT;
            draw_rounded_rect(kx+2, ky+2, bw-4, bh-4, 8, bg);
            int tw2 = text_width(keys[r*4+c], 3);
            draw_text(kx + (bw-tw2)/2, ky+18, keys[r*4+c], COLOR_BLACK, 3);
        }
}

static void calc_handle(int tx, int ty, int released) {
    if (!released) return;
    int bw=(screen_width-40)/4, bh=60;
    for (int r=0; r<4; r++)
        for (int c=0; c<4; c++) {
            int kx=10+c*bw, ky=TOPBAR_H+80+r*bh;
            if (point_in_rect(tx, ty, kx+2, ky+2, bw-4, bh-4)) {
                const char *keys[]={"7","8","9","+","4","5","6","-","1","2","3","*","C","0","=","/"};
                const char *k=keys[r*4+c];
                if (k[0]=='C') { calc_val=0; calc_mem=0; calc_op=0; calc_new=1; strcpy(calc_display,"0"); }
                else if (k[0]=='=') {
                    if (calc_op==1) calc_val=calc_mem+calc_val;
                    else if (calc_op==2) calc_val=calc_mem-calc_val;
                    else if (calc_op==3) calc_val=calc_mem*calc_val;
                    else if (calc_op==4&&calc_val!=0) calc_val=calc_mem/calc_val;
                    snprintf(calc_display, sizeof(calc_display), "%.2f", calc_val);
                    calc_new=1; calc_op=0;
                }
                else if (k[0]=='+'||k[0]=='-'||k[0]=='*'||k[0]=='/') {
                    calc_mem=calc_val;
                    calc_op=(k[0]=='+')?1:(k[0]=='-')?2:(k[0]=='*')?3:4;
                    calc_new=1;
                }
                else {
                    if (calc_new) { calc_val=0; calc_new=0; strcpy(calc_display,""); }
                    int len=strlen(calc_display);
                    calc_display[len]=k[0]; calc_display[len+1]=0;
                    calc_val=atof(calc_display);
                }
                return;
            }
        }
}

static char file_path[256] = "/mnt/us";
static char file_entries[64][128];
static int file_count = 0;
static int file_scroll = 0;

static void file_load(const char *path) {
    strncpy(file_path, path, sizeof(file_path)-1);
    file_count = 0; file_scroll = 0;
    DIR *d = opendir(path); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && file_count < 64) {
        if (e->d_name[0]=='.' && (e->d_name[1]==0||(e->d_name[1]=='.'&&e->d_name[2]==0))) continue;
        strncpy(file_entries[file_count], e->d_name, 127);
        file_count++;
    }
    closedir(d);
}

static void file_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_rounded_rect(x+10, y+10, w-20, 36, 8, COLOR_LIGHT);
    draw_text(x+18, y+16, file_path, COLOR_DARK, 2);
    int iy = y + 56;
    for (int i=file_scroll; i<file_count && i<file_scroll+((h-76)/46); i++) {
        draw_rounded_rect(x+10, iy, w-20, 40, 8, COLOR_LIGHT);
        draw_text(x+20, iy+10, file_entries[i], COLOR_BLACK, 2);
        iy += 46;
    }
}

static void file_handle(int tx, int ty, int released) {
    if (!released) return;
    int iy = TOPBAR_H + 56;
    for (int i=file_scroll; i<file_count; i++) {
        if (point_in_rect(tx, ty, 10, iy, screen_width-20, 40)) {
            char full[512]; snprintf(full, sizeof(full), "%s/%s", file_path, file_entries[i]);
            struct stat st;
            if (stat(full, &st)==0 && S_ISDIR(st.st_mode)) file_load(full);
            else if (strstr(file_entries[i], ".bmp")) draw_bmp(full, 0, 0);
            else if (strstr(file_entries[i], ".pgm")) draw_pgm(full, 0, 0);
            return;
        }
        iy += 46;
    }
    if (ty > TOPBAR_H && ty < TOPBAR_H + 46) {
        char *slash = strrchr(file_path, '/');
        if (slash && slash != file_path) { *slash = 0; file_load(file_path); }
    }
}

static char net_status[64] = "Scanning...";
static char net_ssids[10][64];
static int net_count = 0;

static void net_init(void) {
    strcpy(net_status, "Scanning...");
    net_count = 0;
    FILE *p = popen("iwlist wlan0 scanning 2>/dev/null | grep ESSID", "r");
    if (p) {
        char buf[256];
        while (fgets(buf, sizeof(buf), p) && net_count < 10) {
            char *eq = strchr(buf, '"');
            if (eq) {
                eq++;
                char *end = strchr(eq, '"');
                if (end) { int l=end-eq; strncpy(net_ssids[net_count], eq, l); net_ssids[net_count][l]=0; net_count++; }
            }
        }
        pclose(p);
    }
    snprintf(net_status, sizeof(net_status), "Found %d networks", net_count);
}

static void net_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_text(x+20, y+20, net_status, COLOR_DARK, 2);
    int iy = y + 60;
    for (int i=0; i<net_count && iy<y+h-20; i++) {
        draw_rounded_rect(x+10, iy, w-20, 40, 8, COLOR_LIGHT);
        draw_text(x+20, iy+10, net_ssids[i], COLOR_BLACK, 2);
        iy += 46;
    }
}

static void net_handle(int tx, int ty, int released) {
    if (!released) return;
    int iy = TOPBAR_H + 60;
    for (int i=0; i<net_count; i++) {
        if (point_in_rect(tx, ty, 10, iy, screen_width-20, 40)) {
            char cmd[256]; snprintf(cmd, sizeof(cmd), "echo connecting to %s", net_ssids[i]);
            log_msg(cmd);
            return;
        }
        iy += 46;
    }
}

static char browser_url[256] = "https://";
static int browser_input_active = 0;
static int browser_loading = 0;
static int browser_scroll = 0;

#define BROWSER_MAX_LINES 600
#define BROWSER_MAX_LINKS 80
#define BROWSER_MAX_TABLES 20
#define BROWSER_CACHE "/tmp/kindlejap_browser_cache.html"
#define BROWSER_TEXT_W (screen_width - 20)

typedef enum {
    LINE_TEXT, LINE_H1, LINE_H2, LINE_H3, LINE_H4, LINE_H5, LINE_H6,
    LINE_PARA, LINE_LIST_ITEM, LINE_LINK, LINE_HR, LINE_PRE,
    LINE_BLOCKQUOTE, LINE_TABLE_ROW, LINE_IMAGE, LINE_FORM_INPUT, LINE_FORM_BUTTON,
    LINE_OL_ITEM
} LineType;

typedef struct {
    char *text;
    LineType type;
    int link_idx;
    unsigned char color;
    int bold;
    int italic;
    int underline;
    int font_scale;
} BrowserLine;

static BrowserLine browser_lines[BROWSER_MAX_LINES];
static int browser_line_count = 0;

typedef struct {
    char url[256];
    char label[128];
    int line;
} BrowserLink;

static BrowserLink browser_links[BROWSER_MAX_LINKS];
static int browser_link_count = 0;

typedef struct {
    char cells[8][256];
    int col_count;
    int row_line;
} BrowserTableRow;

typedef struct {
    BrowserTableRow rows[32];
    int row_count;
} BrowserTable;

static BrowserTable browser_tables[BROWSER_MAX_TABLES];
static int browser_table_count = 0;

typedef struct {
    char name[128];
    char value[256];
    char placeholder[128];
    int line;
    int active;
} BrowserFormField;

#define BROWSER_MAX_FORM_FIELDS 16
static BrowserFormField browser_fields[BROWSER_MAX_FORM_FIELDS];
static int browser_field_count = 0;
static int browser_active_field = -1;

static void browser_free_content(void) {
    for (int i = 0; i < browser_line_count; i++)
        if (browser_lines[i].text) { free(browser_lines[i].text); browser_lines[i].text = NULL; }
    browser_line_count = 0;
    browser_link_count = 0;
    browser_table_count = 0;
    browser_field_count = 0;
    browser_active_field = -1;
    browser_scroll = 0;
}

static int browser_emit(LineType type, const char *text, unsigned char color, int bold, int italic, int underline, int font_scale) {
    if (browser_line_count >= BROWSER_MAX_LINES || !text) return -1;
    if (strlen(text) == 0 && type != LINE_HR && type != LINE_PARA && type != LINE_IMAGE) return -1;
    browser_lines[browser_line_count].text = strdup(text);
    browser_lines[browser_line_count].type = type;
    browser_lines[browser_line_count].link_idx = -1;
    browser_lines[browser_line_count].color = color;
    browser_lines[browser_line_count].bold = bold;
    browser_lines[browser_line_count].italic = italic;
    browser_lines[browser_line_count].underline = underline;
    browser_lines[browser_line_count].font_scale = font_scale;
    return browser_line_count++;
}

static void browser_emit_empty(void) {
    if (browser_line_count >= BROWSER_MAX_LINES) return;
    browser_lines[browser_line_count].text = strdup("");
    browser_lines[browser_line_count].type = LINE_PARA;
    browser_lines[browser_line_count].link_idx = -1;
    browser_lines[browser_line_count].color = COLOR_BLACK;
    browser_lines[browser_line_count].bold = 0;
    browser_lines[browser_line_count].italic = 0;
    browser_lines[browser_line_count].underline = 0;
    browser_lines[browser_line_count].font_scale = 1;
    browser_line_count++;
}

static int browser_max_chars(int scale) {
    return BROWSER_TEXT_W / (FONT_W * scale);
}

static void browser_flush_text(char *buf, int *col, LineType forced_type, unsigned char color, int bold, int italic, int underline, int font_scale) {
    if (*col <= 0) return;
    buf[*col] = 0;
    LineType lt = forced_type;
    if (lt == LINE_TEXT && font_scale >= 3) lt = LINE_H1;
    else if (lt == LINE_TEXT && font_scale >= 2) lt = LINE_H2;
    browser_emit(lt, buf, color, bold, italic, underline, font_scale);
    *col = 0;
}

static void browser_parse_html(const char *html) {
    browser_free_content();
    char text_buf[2048] = "";
    int tcol = 0;
    const char *p = html;
    int in_tag = 0;
    int skip_content = 0;
    char tag_name[64] = "";
    int tag_len = 0;
    char tag_attrs[512] = "";
    int attr_len = 0;
    int in_tag_attrs = 0;

    unsigned char cur_color = COLOR_BLACK;
    int cur_bold = 0;
    int cur_italic = 0;
    int cur_underline = 0;
    int cur_scale = 1;
    int link_active = 0;
    int in_pre = 0;
    int in_blockquote = 0;
    int in_table = 0;
    int in_tr = 0;
    int in_td = 0;
    int table_idx = -1;
    int td_idx = 0;
    char td_buf[256] = "";
    int td_col = 0;
    int ol_counter = 0;
    int in_ul = 0;
    int in_ol = 0;

    #define FLUSH_TEXT() browser_flush_text(text_buf, &tcol, LINE_TEXT, cur_color, cur_bold, cur_italic, cur_underline, cur_scale)

    #define PUSH_CHAR(c) do { \
        int maxc = browser_max_chars(cur_scale); \
        if (tcol < (int)sizeof(text_buf)-2) text_buf[tcol++] = (c); \
        if (!in_pre && tcol >= maxc) { \
            int last_sp = -1; \
            for (int wi = tcol-1; wi > tcol-40 && wi >= 0; wi--) \
                if (text_buf[wi] == ' ') { last_sp = wi; break; } \
            if (last_sp > 0) { \
                text_buf[last_sp] = 0; \
                browser_emit(LINE_TEXT, text_buf, cur_color, cur_bold, cur_italic, cur_underline, cur_scale); \
                int rem = tcol - last_sp - 1; \
                memmove(text_buf, text_buf + last_sp + 1, rem); \
                tcol = rem; \
            } else { \
                browser_emit(LINE_TEXT, text_buf, cur_color, cur_bold, cur_italic, cur_underline, cur_scale); \
                tcol = 0; \
            } \
        } \
    } while(0)

    while (*p && browser_line_count < BROWSER_MAX_LINES) {
        if (*p == '<') {
            if (tcol > 0 && !skip_content) {
                text_buf[tcol] = 0;
                if (link_active && browser_link_count < BROWSER_MAX_LINKS) {
                    strncpy(browser_links[browser_link_count].label, text_buf, 127);
                }
            }
            in_tag = 1;
            in_tag_attrs = 0;
            tag_len = 0;
            attr_len = 0;
            tag_name[0] = 0;
            tag_attrs[0] = 0;
            p++;
            continue;
        }
        if (*p == '>') {
            in_tag = 0;
            tag_name[tag_len] = 0;
            tag_attrs[attr_len] = 0;

            /* lowercase tag name */
            for (int ci = 0; ci < tag_len; ci++)
                if (tag_name[ci] >= 'A' && tag_name[ci] <= 'Z') tag_name[ci] += 32;

            int is_closing = (tag_name[0] == '/');
            char *tname = is_closing ? tag_name + 1 : tag_name;

            /* handle table cell content on td/th close */
            if (in_td && is_closing && (strcmp(tname, "td") == 0 || strcmp(tname, "th") == 0)) {
                td_buf[td_col] = 0;
                if (table_idx >= 0 && table_idx < browser_table_count && in_tr) {
                    BrowserTable *tb = &browser_tables[table_idx];
                    if (tb->row_count > 0 && td_idx < 8) {
                        strncpy(tb->rows[tb->row_count-1].cells[td_idx], td_buf, 255);
                        td_idx++;
                        tb->rows[tb->row_count-1].col_count = td_idx;
                    }
                }
                td_col = 0;
                in_td = 0;
            }

            /* handle tr close */
            if (is_closing && strcmp(tname, "tr") == 0) {
                in_tr = 0;
            }

            /* handle table close */
            if (is_closing && strcmp(tname, "table") == 0) {
                in_table = 0;
            }

            /* opening tags */
            if (!is_closing) {
                int is_block = (strcmp(tname, "p") == 0 || strcmp(tname, "div") == 0 ||
                    strcmp(tname, "br") == 0 || strcmp(tname, "li") == 0 ||
                    strcmp(tname, "tr") == 0 || strcmp(tname, "hr") == 0 ||
                    strcmp(tname, "h1") == 0 || strcmp(tname, "h2") == 0 ||
                    strcmp(tname, "h3") == 0 || strcmp(tname, "h4") == 0 ||
                    strcmp(tname, "h5") == 0 || strcmp(tname, "h6") == 0 ||
                    strcmp(tname, "pre") == 0 || strcmp(tname, "blockquote") == 0 ||
                    strcmp(tname, "ul") == 0 || strcmp(tname, "ol") == 0 ||
                    strcmp(tname, "table") == 0 || strcmp(tname, "td") == 0 ||
                    strcmp(tname, "th") == 0 || strcmp(tname, "thead") == 0 ||
                    strcmp(tname, "tbody") == 0);

                if (tcol > 0 && !skip_content && is_block) FLUSH_TEXT();

                if (strcmp(tname, "br") == 0) { FLUSH_TEXT(); tcol = 0; }
                if (strcmp(tname, "hr") == 0) {
                    FLUSH_TEXT();
                    browser_emit(LINE_HR, "---", COLOR_MID, 0, 0, 0, 1);
                }

                /* headings */
                if (strcmp(tname, "h1") == 0) { if (browser_line_count > 0) browser_emit_empty(); cur_scale = 3; cur_bold = 1; }
                else if (strcmp(tname, "h2") == 0) { if (browser_line_count > 0) browser_emit_empty(); cur_scale = 2; cur_bold = 1; }
                else if (strcmp(tname, "h3") == 0 || strcmp(tname, "h4") == 0) { if (browser_line_count > 0) browser_emit_empty(); cur_scale = 2; cur_bold = 0; }
                else if (strcmp(tname, "h5") == 0 || strcmp(tname, "h6") == 0) { cur_scale = 1; cur_bold = 1; }

                /* pre */
                if (strcmp(tname, "pre") == 0) { in_pre = 1; FLUSH_TEXT(); }

                /* blockquote */
                if (strcmp(tname, "blockquote") == 0) { in_blockquote = 1; FLUSH_TEXT(); }

                /* lists */
                if (strcmp(tname, "ul") == 0) { in_ul = 1; FLUSH_TEXT(); }
                if (strcmp(tname, "ol") == 0) { in_ol = 1; ol_counter = 1; FLUSH_TEXT(); }
                if (strcmp(tname, "li") == 0) {
                    FLUSH_TEXT();
                    if (in_ol) {
                        char obuf[16];
                        snprintf(obuf, sizeof(obuf), "%d. ", ol_counter++);
                        for (const char *oi = obuf; *oi; oi++) PUSH_CHAR(*oi);
                    } else if (in_ul) {
                        PUSH_CHAR(0xe2); PUSH_CHAR(0x80); PUSH_CHAR(0xa2); PUSH_CHAR(' ');
                    }
                }

                /* table */
                if (strcmp(tname, "table") == 0) {
                    FLUSH_TEXT();
                    if (browser_table_count < BROWSER_MAX_TABLES) {
                        table_idx = browser_table_count++;
                        browser_tables[table_idx].row_count = 0;
                        in_table = 1;
                    }
                }
                if (strcmp(tname, "tr") == 0 && in_table) {
                    in_tr = 1;
                    td_idx = 0;
                    if (table_idx >= 0 && browser_tables[table_idx].row_count < 32) {
                        memset(&browser_tables[table_idx].rows[browser_tables[table_idx].row_count], 0, sizeof(BrowserTableRow));
                        browser_tables[table_idx].rows[browser_tables[table_idx].row_count].row_line = browser_line_count;
                        browser_tables[table_idx].row_count++;
                    }
                }
                if ((strcmp(tname, "td") == 0 || strcmp(tname, "th") == 0) && in_table) {
                    in_td = 1;
                    td_col = 0;
                }

                /* img */
                if (strcmp(tname, "img") == 0) {
                    FLUSH_TEXT();
                    const char *alt = strstr(tag_attrs, "alt=\"");
                    char alt_buf[128] = "image";
                    if (alt) {
                        alt += 5;
                        const char *end = strchr(alt, '"');
                        if (end && (end - alt) > 0 && (end - alt) < 127) {
                            int al = end - alt;
                            strncpy(alt_buf, alt, al);
                            alt_buf[al] = 0;
                        }
                    }
                    char img_line[256];
                    snprintf(img_line, sizeof(img_line), "[ %s ]", alt_buf);
                    browser_emit(LINE_IMAGE, img_line, COLOR_MID, 0, 0, 0, 1);
                }

                /* form input */
                if (strcmp(tname, "input") == 0) {
                    FLUSH_TEXT();
                    const char *tp = strstr(tag_attrs, "type=\"");
                    const char *nm = strstr(tag_attrs, "name=\"");
                    const char *pl = strstr(tag_attrs, "placeholder=\"");
                    const char *vl = strstr(tag_attrs, "value=\"");
                    char ftype[32] = "text";
                    char fname[128] = "";
                    char fph[128] = "";
                    char fval[256] = "";
                    if (tp) { tp += 6; const char *e = strchr(tp, '"'); if (e) { int l = e-tp; if (l<31) { strncpy(ftype, tp, l); ftype[l]=0; } } }
                    if (nm) { nm += 6; const char *e = strchr(nm, '"'); if (e) { int l = e-nm; if (l<127) { strncpy(fname, nm, l); fname[l]=0; } } }
                    if (pl) { pl += 13; const char *e = strchr(pl, '"'); if (e) { int l = e-pl; if (l<127) { strncpy(fph, pl, l); fph[l]=0; } } }
                    if (vl) { vl += 7; const char *e = strchr(vl, '"'); if (e) { int l = e-vl; if (l<255) { strncpy(fval, vl, l); fval[l]=0; } } }
                    if (strcmp(ftype, "submit") == 0 || strcmp(ftype, "button") == 0) {
                        int li = browser_emit(LINE_FORM_BUTTON, strlen(fval) > 0 ? fval : "Submit", COLOR_BLACK, 0, 0, 0, 2);
                        if (browser_field_count < BROWSER_MAX_FORM_FIELDS && li >= 0) {
                            strncpy(browser_fields[browser_field_count].name, fname, 127);
                            strncpy(browser_fields[browser_field_count].value, fval, 255);
                            browser_fields[browser_field_count].line = li;
                            browser_field_count++;
                        }
                    } else {
                        char finp[512];
                        snprintf(finp, sizeof(finp), "[%s]", strlen(fval) > 0 ? fval : (strlen(fph) > 0 ? fph : fname));
                        int li = browser_emit(LINE_FORM_INPUT, finp, COLOR_DARK, 0, 0, 0, 1);
                        if (browser_field_count < BROWSER_MAX_FORM_FIELDS && li >= 0) {
                            strncpy(browser_fields[browser_field_count].name, fname, 127);
                            strncpy(browser_fields[browser_field_count].value, fval, 255);
                            strncpy(browser_fields[browser_field_count].placeholder, fph, 127);
                            browser_fields[browser_field_count].line = li;
                            browser_field_count++;
                        }
                    }
                }

                /* button */
                if (strcmp(tname, "button") == 0) { FLUSH_TEXT(); }

                /* link */
                if (strcmp(tname, "a") == 0 && browser_link_count < BROWSER_MAX_LINKS) {
                    const char *href = strstr(tag_attrs, "href=\"");
                    if (href) {
                        href += 6;
                        const char *end = strchr(href, '"');
                        if (end && (end - href) > 0 && (end - href) < 255) {
                            int l = end - href;
                            strncpy(browser_links[browser_link_count].url, href, l);
                            browser_links[browser_link_count].url[l] = 0;
                            browser_links[browser_link_count].line = browser_line_count;
                            link_active = 1;
                        }
                    }
                }

                /* inline styles */
                if (strcmp(tname, "b") == 0 || strcmp(tname, "strong") == 0) cur_bold = 1;
                if (strcmp(tname, "i") == 0 || strcmp(tname, "em") == 0) cur_italic = 1;
                if (strcmp(tname, "u") == 0) cur_underline = 1;
                if (strcmp(tname, "s") == 0 || strcmp(tname, "del") == 0 || strcmp(tname, "strike") == 0) cur_underline = 1;

                /* font color */
                if (strcmp(tname, "font") == 0) {
                    const char *clr = strstr(tag_attrs, "color=\"");
                    if (clr) {
                        clr += 7;
                        if (strncasecmp(clr, "red", 3) == 0) cur_color = COLOR_DARK;
                        else if (strncasecmp(clr, "blue", 4) == 0) cur_color = COLOR_DARK;
                        else if (strncasecmp(clr, "gray", 4) == 0 || strncasecmp(clr, "grey", 4) == 0) cur_color = COLOR_MID;
                        else if (strncasecmp(clr, "green", 5) == 0) cur_color = COLOR_DARK;
                    }
                }
                if (strcmp(tname, "span") == 0) {
                    const char *sty = strstr(tag_attrs, "style=\"");
                    if (sty) {
                        sty += 7;
                        if (strstr(sty, "color:")) {
                            const char *cv = strstr(sty, "color:");
                            cv += 6;
                            while (*cv == ' ') cv++;
                            if (strncasecmp(cv, "red", 3) == 0) cur_color = COLOR_DARK;
                            else if (strncasecmp(cv, "blue", 4) == 0) cur_color = COLOR_DARK;
                            else if (strncasecmp(cv, "gray", 4) == 0 || strncasecmp(cv, "#888", 4) == 0 || strncasecmp(cv, "#999", 4) == 0) cur_color = COLOR_MID;
                            else if (strncasecmp(cv, "#aaa", 4) == 0 || strncasecmp(cv, "#bbb", 4) == 0) cur_color = COLOR_MID;
                        }
                        if (strstr(sty, "font-weight: bold")) cur_bold = 1;
                        if (strstr(sty, "font-style: italic")) cur_italic = 1;
                        if (strstr(sty, "text-decoration: underline")) cur_underline = 1;
                    }
                }

                /* skip style/script/noscript */
                if (strcmp(tname, "style") == 0 || strcmp(tname, "script") == 0 || strcmp(tname, "noscript") == 0)
                    skip_content = 1;

                /* title -> emit as h1 */
                if (strcmp(tname, "title") == 0) { skip_content = 1; }
            }

            /* closing tags */
            if (is_closing) {
                if (tcol > 0 && !skip_content) {
                    text_buf[tcol] = 0;
                    LineType lt = LINE_TEXT;
                    if (link_active && browser_link_count < BROWSER_MAX_LINKS) {
                        strncpy(browser_links[browser_link_count].label, text_buf, 127);
                        lt = LINE_LINK;
                        browser_lines[browser_line_count].link_idx = browser_link_count;
                        browser_link_count++;
                        link_active = 0;
                    }
                    if (strcmp(tname, "/h1") == 0) { lt = LINE_H1; cur_scale = 3; cur_bold = 1; }
                    else if (strcmp(tname, "/h2") == 0) { lt = LINE_H2; cur_scale = 2; cur_bold = 1; }
                    else if (strcmp(tname, "/h3") == 0 || strcmp(tname, "/h4") == 0) { lt = LINE_H3; cur_scale = 2; }
                    else if (strcmp(tname, "/h5") == 0 || strcmp(tname, "/h6") == 0) { lt = LINE_H5; cur_scale = 1; cur_bold = 1; }
                    else if (strcmp(tname, "/p") == 0) lt = LINE_PARA;
                    else if (strcmp(tname, "/pre") == 0) lt = LINE_PRE;
                    else if (strcmp(tname, "/li") == 0) lt = LINE_LIST_ITEM;
                    else if (strcmp(tname, "/blockquote") == 0) lt = LINE_BLOCKQUOTE;
                    else if (strcmp(tname, "/button") == 0) lt = LINE_FORM_BUTTON;
                    browser_emit(lt, text_buf, cur_color, cur_bold, cur_italic, cur_underline, cur_scale);
                    tcol = 0;
                }

                if (strcmp(tname, "/h1") == 0 || strcmp(tname, "/h2") == 0 ||
                    strcmp(tname, "/h3") == 0 || strcmp(tname, "/h4") == 0 ||
                    strcmp(tname, "/h5") == 0 || strcmp(tname, "/h6") == 0) {
                    browser_emit_empty();
                    cur_scale = 1; cur_bold = 0;
                }
                if (strcmp(tname, "/p") == 0 || strcmp(tname, "/div") == 0) browser_emit_empty();
                if (strcmp(tname, "/pre") == 0) { in_pre = 0; browser_emit_empty(); }
                if (strcmp(tname, "/blockquote") == 0) { in_blockquote = 0; browser_emit_empty(); }
                if (strcmp(tname, "/ul") == 0) { in_ul = 0; }
                if (strcmp(tname, "/ol") == 0) { in_ol = 0; }
                if (strcmp(tname, "/table") == 0) { in_table = 0; table_idx = -1; browser_emit_empty(); }
                if (strcmp(tname, "/title") == 0) skip_content = 0;
                if (strcmp(tname, "/style") == 0 || strcmp(tname, "/script") == 0 || strcmp(tname, "/noscript") == 0) skip_content = 0;

                /* reset inline styles on close */
                if (strcmp(tname, "/b") == 0 || strcmp(tname, "/strong") == 0) cur_bold = 0;
                if (strcmp(tname, "/i") == 0 || strcmp(tname, "/em") == 0) cur_italic = 0;
                if (strcmp(tname, "/u") == 0) cur_underline = 0;
                if (strcmp(tname, "/s") == 0 || strcmp(tname, "/del") == 0 || strcmp(tname, "/strike") == 0) cur_underline = 0;
                if (strcmp(tname, "/font") == 0) cur_color = COLOR_BLACK;
                if (strcmp(tname, "/span") == 0) { /* don't reset - keep inherited */ }
            }
            p++;
            continue;
        }
        if (in_tag) {
            if (tag_len < 63) tag_name[tag_len++] = *p;
            if (in_tag_attrs && attr_len < 511) tag_attrs[attr_len++] = *p;
            if (!in_tag_attrs && tag_len > 0 && tag_name[tag_len-1] == ' ') { in_tag_attrs = 1; tag_len--; }
            p++; continue;
        }
        if (skip_content) { p++; continue; }

        if (in_td) {
            if (*p == '\n' || *p == '\r' || *p == '\t') { p++; continue; }
            if (*p == ' ' && td_col > 0 && td_buf[td_col-1] == ' ') { p++; continue; }
            if (td_col < 255) td_buf[td_col++] = *p;
            p++; continue;
        }

        if (*p == '&') {
            char ent = 0;
            int ent_skip = 0;
            if (strncasecmp(p, "&amp;", 5) == 0) { ent = '&'; ent_skip = 5; }
            else if (strncasecmp(p, "&lt;", 4) == 0) { ent = '<'; ent_skip = 4; }
            else if (strncasecmp(p, "&gt;", 4) == 0) { ent = '>'; ent_skip = 4; }
            else if (strncasecmp(p, "&nbsp;", 6) == 0) { ent = ' '; ent_skip = 6; }
            else if (strncasecmp(p, "&quot;", 6) == 0) { ent = '"'; ent_skip = 6; }
            else if (strncasecmp(p, "&apos;", 6) == 0) { ent = '\''; ent_skip = 6; }
            else if (strncasecmp(p, "&mdash;", 7) == 0) { strncpy(text_buf+tcol, "--", 2); tcol += 2; p += 7; continue; }
            else if (strncasecmp(p, "&ndash;", 7) == 0) { ent = '-'; ent_skip = 7; }
            else if (strncasecmp(p, "&hellip;", 8) == 0) { strncpy(text_buf+tcol, "...", 3); tcol += 3; p += 8; continue; }
            else if (strncasecmp(p, "&rsquo;", 7) == 0 || strncasecmp(p, "&lsquo;", 7) == 0) { ent = '\''; ent_skip = 7; }
            else if (strncasecmp(p, "&rdquo;", 7) == 0 || strncasecmp(p, "&ldquo;", 7) == 0) { ent = '"'; ent_skip = 7; }
            else if (strncasecmp(p, "&rarr;", 6) == 0) { strncpy(text_buf+tcol, "->", 2); tcol += 2; p += 6; continue; }
            else if (strncasecmp(p, "&larr;", 6) == 0) { strncpy(text_buf+tcol, "<-", 2); tcol += 2; p += 6; continue; }
            else if (strncasecmp(p, "&times;", 7) == 0) { ent = 'x'; ent_skip = 7; }
            else if (strncasecmp(p, "&divide;", 8) == 0) { ent = '/'; ent_skip = 8; }
            else if (strncasecmp(p, "&copy;", 6) == 0) { strncpy(text_buf+tcol, "(c)", 3); tcol += 3; p += 6; continue; }
            else if (strncasecmp(p, "&reg;", 5) == 0) { strncpy(text_buf+tcol, "(R)", 3); tcol += 3; p += 5; continue; }
            else if (strncasecmp(p, "&euro;", 6) == 0) { ent = '$'; ent_skip = 6; }
            else if (strncasecmp(p, "&pound;", 7) == 0) { ent = '#'; ent_skip = 7; }
            else if (strncasecmp(p, "&yen;", 5) == 0) { ent = 'Y'; ent_skip = 5; }
            else {
                /* numeric entity &#123; or &#x1B; */
                if (p[1] == '#' && (p[2] == 'x' || p[2] == 'X')) {
                    unsigned int code = 0;
                    const char *hex = p + 3;
                    while (*hex && *hex != ';') {
                        code *= 16;
                        if (*hex >= '0' && *hex <= '9') code += *hex - '0';
                        else if (*hex >= 'a' && *hex <= 'f') code += *hex - 'a' + 10;
                        else if (*hex >= 'A' && *hex <= 'F') code += *hex - 'A' + 10;
                        hex++;
                    }
                    if (code < 128 && code > 0) ent = (char)code;
                    ent_skip = hex - p + 1;
                } else if (p[1] == '#') {
                    unsigned int code = 0;
                    const char *dec = p + 2;
                    while (*dec && *dec != ';') { code = code * 10 + (*dec - '0'); dec++; }
                    if (code < 128 && code > 0) ent = (char)code;
                    ent_skip = dec - p + 1;
                } else {
                    PUSH_CHAR(*p); p++; continue;
                }
            }
            if (ent) { PUSH_CHAR(ent); p += ent_skip; }
            else { PUSH_CHAR(*p); p++; }
            continue;
        }

        if (in_pre) {
            if (*p == '\n' || *p == '\r') {
                FLUSH_TEXT();
                p++;
                continue;
            }
            PUSH_CHAR(*p);
            p++;
            continue;
        }

        if (*p == '\n' || *p == '\r') { p++; continue; }
        if (tcol == 0 && *p == ' ') { p++; continue; }
        if (*p == '\t') { PUSH_CHAR(' '); PUSH_CHAR(' '); PUSH_CHAR(' '); p++; continue; }

        /* collapse multiple spaces */
        if (*p == ' ' && tcol > 0 && text_buf[tcol-1] == ' ') { p++; continue; }

        PUSH_CHAR(*p);
        p++;
    }

    if (tcol > 0 && browser_line_count < BROWSER_MAX_LINES) {
        text_buf[tcol] = 0;
        LineType lt = LINE_TEXT;
        if (link_active && browser_link_count < BROWSER_MAX_LINKS) {
            strncpy(browser_links[browser_link_count].label, text_buf, 127);
            lt = LINE_LINK;
            browser_lines[browser_line_count].link_idx = browser_link_count;
            browser_link_count++;
        }
        browser_emit(lt, text_buf, cur_color, cur_bold, cur_italic, cur_underline, cur_scale);
    }

    /* emit tables as formatted rows */
    for (int ti = 0; ti < browser_table_count; ti++) {
        BrowserTable *tb = &browser_tables[ti];
        if (tb->row_count == 0) continue;
        /* separator before table */
        browser_emit(LINE_HR, "", COLOR_MID, 0, 0, 0, 1);
        for (int ri = 0; ri < tb->row_count; ri++) {
            BrowserTableRow *tr = &tb->rows[ri];
            if (tr->col_count == 0) continue;
            char row_buf[1024] = "";
            int rcol = 0;
            for (int ci = 0; ci < tr->col_count && ci < 8; ci++) {
                int cell_len = strlen(tr->cells[ci]);
                if (ci > 0 && rcol < 1022) row_buf[rcol++] = '|';
                for (int cj = 0; cj < cell_len && rcol < 1020; cj++)
                    row_buf[rcol++] = tr->cells[ci][cj];
                /* pad to fixed width */
                int target = (ci < tr->col_count - 1) ? 24 : 0;
                while (rcol < target + (ci > 0 ? rcol : 0) && rcol < 1020) row_buf[rcol++] = ' ';
            }
            row_buf[rcol] = 0;
            browser_emit(LINE_TABLE_ROW, row_buf, COLOR_BLACK, ri == 0, 0, 0, 1);
        }
        browser_emit(LINE_HR, "", COLOR_MID, 0, 0, 0, 1);
    }
}

static void browser_load_url(const char *url) {
    browser_loading = 1;
    browser_scroll = 0;
    dirty = 1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -sL -A \"KindleJap/2.0\" --max-time 15 \"%s\" -o " BROWSER_CACHE " 2>/dev/null", url);
    system(cmd);
    FILE *f = fopen(BROWSER_CACHE, "r");
    if (!f) { browser_loading = 0; snprintf(browser_url, sizeof(browser_url), "%s", url); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 500000) sz = 500000;
    char *html = malloc(sz + 1);
    if (html) {
        int n = fread(html, 1, sz, f);
        html[n] = 0;
        browser_parse_html(html);
        free(html);
    }
    fclose(f);
    snprintf(browser_url, sizeof(browser_url), "%s", url);
    browser_loading = 0;
    dirty = 1;
}

static void browser_draw_styled_char(int x, int y, char ch, unsigned char color, int scale, int bold, int italic) {
    draw_char(x, y, ch, color, scale);
    if (bold) draw_char(x+1, y, ch, color, scale);
    if (italic) draw_char(x+1, y+1, ch, color, scale);
}

static void browser_draw_styled_text(int x, int y, const char *t, unsigned char color, int scale, int bold, int italic, int underline) {
    int cx = x;
    int len = 0;
    const char *s = t;
    while (*s) { len++; s++; }
    while (*t) {
        browser_draw_styled_char(cx, y, *t, color, scale, bold, italic);
        cx += FONT_W * scale;
        t++;
    }
    if (underline) {
        int tw = len * FONT_W * scale;
        draw_rect(x, y + FONT_H * scale, tw, 1, color);
    }
}

static void browser_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);

    draw_rounded_rect(x+10, y+10, w-20, 36, 8, COLOR_LIGHT);
    unsigned char url_color = browser_input_active ? COLOR_BLACK : COLOR_DARK;
    draw_text(x+18, y+16, browser_url, url_color, 1);
    if (browser_input_active) {
        int cw = text_width("_", 1);
        int ux = x + 18 + text_width(browser_url, 1);
        draw_text(ux, y+16, "_", COLOR_BLACK, 1);
    }

    if (browser_loading) {
        draw_text_centered_in(x, y + h/2, w, "Loading...", COLOR_MID, 3);
        return;
    }
    if (browser_line_count == 0) {
        if (browser_input_active)
            draw_text_centered_in(x, y + h/2, w, "Type URL and press Enter", COLOR_MID, 2);
        else
            draw_text_centered_in(x, y + h/2, w, "Tap URL bar to load", COLOR_MID, 2);
        return;
    }

    int iy = y + 56;
    int max_y = y + h - 44;
    int max_scroll = browser_line_count;
    if (browser_scroll >= max_scroll - 5) browser_scroll = max_scroll - 5;
    if (browser_scroll < 0) browser_scroll = 0;

    for (int i = browser_scroll; i < browser_line_count && iy < max_y; i++) {
        BrowserLine *bl = &browser_lines[i];
        if (!bl->text) continue;

        unsigned char color = bl->color;
        int scale = bl->font_scale;
        int bold = bl->bold;
        int italic = bl->italic;
        int underline = bl->underline;

        int line_h = FONT_H * scale + 4;

        if (bl->type == LINE_PARA && strlen(bl->text) == 0) {
            iy += 8;
            continue;
        }

        if (bl->type == LINE_HR) {
            iy += 4;
            draw_rect(x+10, iy, w-20, 2, COLOR_MID);
            iy += 8;
            continue;
        }

        if (bl->type == LINE_IMAGE) {
            draw_rounded_rect(x+10, iy, w-20, 40, 6, COLOR_LIGHT);
            draw_text_centered_in(x, iy+12, w, bl->text, COLOR_MID, 1);
            iy += 48;
            continue;
        }

        if (bl->type == LINE_TABLE_ROW) {
            int tw2 = text_width(bl->text, 1);
            draw_rect(x+10, iy, tw2 + 8, line_h - 2, COLOR_WHITE);
            draw_rect(x+10, iy, tw2 + 8, line_h - 2, COLOR_LIGHT);
            draw_rect(x+10, iy, tw2 + 8, 1, COLOR_MID);
            browser_draw_styled_text(x+14, iy+2, bl->text, color, scale, bold, italic, underline);
            iy += line_h;
            continue;
        }

        if (bl->type == LINE_FORM_INPUT) {
            draw_rounded_rect(x+10, iy, w-20, 28, 6, COLOR_LIGHT);
            draw_rect(x+12, iy+2, w-24, 24, COLOR_WHITE);
            browser_draw_styled_text(x+18, iy+6, bl->text, COLOR_DARK, 1, 0, 0, 0);
            iy += 32;
            continue;
        }

        if (bl->type == LINE_FORM_BUTTON) {
            int bw2 = text_width(bl->text, 2) + 24;
            int bx = x + (w - bw2) / 2;
            draw_rounded_rect(bx, iy, bw2, 30, 8, COLOR_DARK);
            draw_text_centered_in(bx, iy+6, bw2, bl->text, COLOR_WHITE, 2);
            iy += 36;
            continue;
        }

        if (bl->type == LINE_BLOCKQUOTE) {
            draw_rect(x+10, iy, 4, line_h - 2, COLOR_MID);
            browser_draw_styled_text(x+20, iy+2, bl->text, COLOR_DARK, scale, bold, italic, underline);
            iy += line_h;
            continue;
        }

        if (bl->type == LINE_LIST_ITEM || bl->type == LINE_OL_ITEM) {
            browser_draw_styled_text(x+14, iy+2, bl->text, color, scale, bold, italic, underline);
            iy += line_h;
            continue;
        }

        if (bl->type == LINE_PRE) {
            draw_rect(x+8, iy-1, w-16, line_h, COLOR_LIGHT);
            browser_draw_styled_text(x+14, iy+2, bl->text, COLOR_BLACK, 1, 0, 0, 0);
            iy += line_h;
            continue;
        }

        if (bl->type == LINE_LINK) {
            browser_draw_styled_text(x+14, iy+2, bl->text, COLOR_DARK, scale, 0, 0, 0);
            int lw = text_width(bl->text, scale);
            draw_rect(x+14, iy + 2 + FONT_H * scale, lw, 1, COLOR_DARK);
            iy += line_h;
            continue;
        }

        browser_draw_styled_text(x+14, iy+2, bl->text, color, scale, bold, italic, underline);
        iy += line_h;
    }

    if (browser_scroll > 0) {
        draw_rounded_rect(x+w-44, y+h-44, 34, 34, 8, COLOR_LIGHT);
        draw_text_centered_in(x+w-44, y+h-38, 34, "^", COLOR_BLACK, 2);
    }
    if (browser_scroll < max_scroll - 5) {
        draw_rounded_rect(x+10, y+h-44, 34, 34, 8, COLOR_LIGHT);
        draw_text_centered_in(x+10, y+h-38, 34, "v", COLOR_BLACK, 2);
    }

    int bar_w = 8;
    int bar_h = (max_scroll > 0) ? (h - 56 - 48) * 10 / max_scroll : 0;
    if (bar_h < 20) bar_h = 20;
    if (bar_h > h - 56 - 48) bar_h = h - 56 - 48;
    int bar_y = y + 56 + (max_scroll > 0 ? (h - 56 - 48) * browser_scroll / max_scroll : 0);
    draw_rect(x + w - 6, bar_y, bar_w, bar_h, COLOR_MID);
}

static void browser_handle(int tx, int ty, int released) {
    if (!released) return;
    if (point_in_rect(tx, ty, 10, TOPBAR_H+10, screen_width-20, 36)) {
        keyboard_visible = 1;
        keyboard_mode = 0;
        keyboard_cursor = strlen(browser_url);
        strcpy(keyboard_buf, browser_url);
        browser_input_active = 1;
        return;
    }
    if (browser_loading) return;

    if (browser_scroll < 0) browser_scroll = 0;

    int iy = TOPBAR_H + 56;
    for (int i = browser_scroll; i < browser_line_count && iy < screen_height - TOPBAR_H - 44; i++) {
        BrowserLine *bl = &browser_lines[i];
        if (!bl->text) continue;
        int line_h;
        switch (bl->type) {
            case LINE_H1: line_h = FONT_H * 3 + 8; break;
            case LINE_H2: case LINE_H3: case LINE_H4: line_h = FONT_H * 2 + 6; break;
            case LINE_IMAGE: line_h = 48; break;
            case LINE_FORM_INPUT: line_h = 32; break;
            case LINE_FORM_BUTTON: line_h = 36; break;
            case LINE_PARA: if (strlen(bl->text) == 0) { iy += 8; continue; } line_h = 16; break;
            default: line_h = FONT_H * (bl->font_scale > 0 ? bl->font_scale : 1) + 4; break;
        }

        if (ty >= iy && ty < iy + line_h) {
            /* link tap */
            if (bl->link_idx >= 0 && bl->link_idx < browser_link_count) {
                BrowserLink *lk = &browser_links[bl->link_idx];
                char full[512] = "";
                if (strncmp(lk->url, "http", 4) == 0)
                    strncpy(full, lk->url, sizeof(full)-1);
                else if (lk->url[0] == '/') {
                    const char *host = strstr(browser_url, "://");
                    if (host) {
                        host += 3;
                        const char *slash = strchr(host, '/');
                        int hlen = slash ? (slash - host) : strlen(host);
                        snprintf(full, sizeof(full), "%.*s://%.*s%s", (int)(host - 3 - browser_url), browser_url, hlen, host, lk->url);
                    }
                }
                if (strlen(full) > 0) browser_load_url(full);
                return;
            }
            /* form input tap */
            if (bl->type == LINE_FORM_INPUT) {
                for (int fi = 0; fi < browser_field_count; fi++) {
                    if (browser_fields[fi].line == i) {
                        browser_active_field = fi;
                        keyboard_visible = 1;
                        keyboard_mode = 0;
                        strcpy(keyboard_buf, browser_fields[fi].value);
                        keyboard_cursor = strlen(keyboard_buf);
                        browser_input_active = 1;
                        return;
                    }
                }
            }
            /* form button tap */
            if (bl->type == LINE_FORM_BUTTON) {
                dirty = 1;
                return;
            }
        }
        iy += line_h;
    }

    if (point_in_rect(tx, ty, screen_width-44, screen_height-TOPBAR_H-44, 34, 34) && browser_scroll > 0) {
        browser_scroll -= 5; dirty = 1; return;
    }
    if (point_in_rect(tx, ty, 10, screen_height-TOPBAR_H-44, 34, 34) && browser_scroll < browser_line_count) {
        browser_scroll += 5; dirty = 1; return;
    }
}

static int setup_active = 0;
static int setup_step = 0;
static int setup_tutorial_step = 0;
static int setup_tutorial_done = 0;
static char kindle_model[128] = "Kindle";
static char kindle_fw[64] = "";
static char kindle_screen[64] = "";

static void setup_detect_device(void) {
    FILE *f;
    char line[256];
    f = fopen("/proc/version", "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            snprintf(kindle_fw, sizeof(kindle_fw), "%s", line);
            kindle_fw[strcspn(kindle_fw, "\n")] = 0;
        }
        fclose(f);
    }
    f = fopen("/etc/issue", "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(kindle_model, sizeof(kindle_model), "%s", line);
        }
        fclose(f);
    }
    if (strlen(kindle_model) <= 1) {
        f = popen("cat /sys/devices/soc0/soc_id 2>/dev/null", "r");
        if (f) {
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                int id = atoi(line);
                switch (id) {
                    case 4: snprintf(kindle_model, sizeof(kindle_model), "Kindle Paperwhite 1"); break;
                    case 8: snprintf(kindle_model, sizeof(kindle_model), "Kindle Paperwhite 2"); break;
                    case 24: snprintf(kindle_model, sizeof(kindle_model), "Kindle Paperwhite 3 (PW3)"); break;
                    case 28: snprintf(kindle_model, sizeof(kindle_model), "Kindle Paperwhite 4 (PW4)"); break;
                    case 272: snprintf(kindle_model, sizeof(kindle_model), "Kindle Paperwhite 5 (PW5)"); break;
                    case 11: snprintf(kindle_model, sizeof(kindle_model), "Kindle Voyage"); break;
                    case 10: snprintf(kindle_model, sizeof(kindle_model), "Kindle 7"); break;
                    case 13: snprintf(kindle_model, sizeof(kindle_model), "Kindle 8"); break;
                    case 233: snprintf(kindle_model, sizeof(kindle_model), "Kindle 10"); break;
                    default: snprintf(kindle_model, sizeof(kindle_model), "Kindle (SOC ID: %d)", id); break;
                }
            }
            pclose(f);
        }
    }
    snprintf(kindle_screen, sizeof(kindle_screen), "%dx%d", screen_width, screen_height);
}

static int setup_is_done(void) {
    FILE *f = fopen(SETUP_DONE_FILE, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void setup_mark_done(void) {
    FILE *f = fopen(SETUP_DONE_FILE, "w");
    if (f) { fprintf(f, "done=1\n"); fclose(f); }
}

static void setup_begin(void) {
    setup_active = 1;
    setup_step = 0;
    setup_tutorial_step = 0;
    setup_tutorial_done = 0;
    setup_detect_device();
    dirty = 1;
}

static void setup_finish(void) {
    setup_active = 0;
    setup_mark_done();
    dirty = 1;
    log_msg("Setup completed");
}

#define TUTORIAL_STEPS 6
static const char *tutorial_titles[] = {
    "The Top Bar",
    "The Menu",
    "Sleep Mode",
    "Opening Apps",
    "Notifications",
    "Scrolling"
};
static const char *tutorial_texts[] = {
    "The top bar shows WiFi status on the left and battery percentage on the right.\n\nTap the top-right area to open the notification sidebar.",
    "Press the power button to open the control menu.\n\nFrom here you can open the main menu, put the device to sleep, or exit KindleJap.",
    "Press power once to see the menu, then tap 'Sleep' to put the device to sleep.\n\nThe Kindle UI is preserved and will return when you wake up.",
    "The main menu shows all installed apps.\n\nTap any app name to open it. You can install more apps from the Package Manager.",
    "The notification sidebar shows alerts from apps.\n\nTap the top-right corner of the screen to toggle it. Tap a notification action to respond.",
    "Use the arrow buttons at the bottom of the screen to scroll through content.\n\nSwipe or tap ^ and v to navigate up and down."
};
static int tutorial_highlight_x[TUTORIAL_STEPS];
static int tutorial_highlight_y[TUTORIAL_STEPS];
static int tutorial_highlight_w[TUTORIAL_STEPS];
static int tutorial_highlight_h[TUTORIAL_STEPS];
static int tutorial_anim_frame = 0;

static void setup_init_tutorial(void) {
    tutorial_highlight_x[0] = 10; tutorial_highlight_y[0] = 2; tutorial_highlight_w[0] = 200; tutorial_highlight_h[0] = 36;
    tutorial_highlight_x[1] = screen_width/2-50; tutorial_highlight_y[1] = TOPBAR_H+4; tutorial_highlight_w[1] = 100; tutorial_highlight_h[1] = 36;
    tutorial_highlight_x[2] = screen_width/2-50; tutorial_highlight_y[2] = TOPBAR_H+4; tutorial_highlight_w[2] = 100; tutorial_highlight_h[2] = 36;
    tutorial_highlight_x[3] = 100; tutorial_highlight_y[3] = TOPBAR_H+100; tutorial_highlight_w[3] = screen_width-200; tutorial_highlight_h[3] = 60;
    tutorial_highlight_x[4] = screen_width-180; tutorial_highlight_y[4] = 2; tutorial_highlight_w[4] = 160; tutorial_highlight_h[4] = 36;
    tutorial_highlight_x[5] = 10; tutorial_highlight_y[5] = screen_height-TOPBAR_H-80; tutorial_highlight_w[5] = 40; tutorial_highlight_h[5] = 40;
}

static void setup_draw_topbar(int tutorial) {
    draw_rect(0, 0, screen_width, TOPBAR_H, COLOR_BLACK);
    draw_text(16, 10, wifi ? "WiFi" : "---", COLOR_WHITE, 2);
    char bstr[16];
    int batt = battery_percent();
    snprintf(bstr, sizeof(bstr), "%d%%", batt);
    int bw = text_width(bstr, 2);
    draw_text(screen_width - bw - 16, 10, bstr, COLOR_WHITE, 2);
    if (!tutorial) {
        int progress_w = 200;
        int progress_x = (screen_width - progress_w) / 2;
        int step = setup_step;
        int total = 2;
        draw_rect(progress_x, 14, progress_w, 8, COLOR_DARK);
        draw_rect(progress_x, 14, progress_w * step / total, 8, COLOR_WHITE);
    }
}

static void setup_draw_buttons(const char *left_label, const char *right_label) {
    int btn_h = 36;
    int btn_w = 160;
    int btn_y = screen_height - TOPBAR_H - btn_h - 20;
    if (left_label) {
        draw_rounded_rect(20, btn_y, btn_w, btn_h, 8, COLOR_MID);
        draw_text_centered_in(20, btn_y + 8, btn_w, left_label, COLOR_WHITE, 2);
    }
    if (right_label) {
        draw_rounded_rect(screen_width - btn_w - 20, btn_y, btn_w, btn_h, 8, COLOR_DARK);
        draw_text_centered_in(screen_width - btn_w - 20, btn_y + 8, btn_w, right_label, COLOR_WHITE, 2);
    }
}

static int setup_draw_step0(void) {
    setup_draw_topbar(0);
    int y = TOPBAR_H + 30;
    draw_text_centered_in(0, y, screen_width, "Welcome to", COLOR_MID, 3);
    y += 42;
    draw_text_centered_in(0, y, screen_width, "KindleJap", COLOR_BLACK, 5);
    y += 70;
    draw_rounded_rect(60, y, screen_width-120, 2, 1, COLOR_LIGHT);
    y += 20;
    draw_text_centered_in(0, y, screen_width, "A custom launcher for your Kindle", COLOR_DARK, 2);
    y += 30;
    draw_text_centered_in(0, y, screen_width, "with apps, package manager,", COLOR_DARK, 1);
    y += 16;
    draw_text_centered_in(0, y, screen_width, "notifications, and more.", COLOR_DARK, 1);
    y += 40;
    draw_rounded_rect(40, y, screen_width-80, 90, 8, COLOR_LIGHT);
    draw_text(60, y+12, "Device:", COLOR_DARK, 2);
    draw_text(60, y+36, kindle_model, COLOR_BLACK, 2);
    draw_text(60, y+60, kindle_screen, COLOR_DARK, 1);
    if (strlen(kindle_fw) > 0) {
        char fw_short[64];
        strncpy(fw_short, kindle_fw, 63);
        fw_short[60] = 0;
        draw_text(260, y+60, fw_short, COLOR_DARK, 1);
    }
    y += 110;
    draw_text_centered_in(0, y, screen_width, "Version " KINDLEJAP_VERSION, COLOR_MID, 1);
    setup_draw_buttons(NULL, "Next");
    return 1;
}

static int setup_draw_step1(void) {
    setup_draw_topbar(0);
    int y = TOPBAR_H + 20;
    draw_text_centered_in(0, y, screen_width, "Instructions", COLOR_BLACK, 3);
    y += 44;
    draw_text_centered_in(0, y, screen_width, "Learn how to use KindleJap", COLOR_DARK, 1);
    y += 30;
    int tx = 30;
    int tw = screen_width - 60;
    draw_rounded_rect(tx, y, tw, 280, 8, COLOR_LIGHT);
    const char *title = tutorial_titles[setup_tutorial_step];
    const char *text = tutorial_texts[setup_tutorial_step];
    draw_text_centered_in(tx, y+12, tw, title, COLOR_BLACK, 2);
    y += 40;
    int ty = y;
    const char *line = text;
    while (*line) {
        const char *nl = strchr(line, '\n');
        int llen = nl ? (nl - line) : (int)strlen(line);
        if (llen > 0 && llen < 256) {
            char lbuf[256];
            memcpy(lbuf, line, llen);
            lbuf[llen] = 0;
            draw_text(tx+16, ty, lbuf, COLOR_DARK, 1);
        }
        ty += 18;
        if (!nl) break;
        line = nl + 1;
        while (*line == '\n') { ty += 8; line++; }
    }
    y += 280 + 10;
    tutorial_anim_frame++;
    if (tutorial_anim_frame > 30) tutorial_anim_frame = 0;
    int hx = tutorial_highlight_x[setup_tutorial_step];
    int hy = tutorial_highlight_y[setup_tutorial_step];
    int hw = tutorial_highlight_w[setup_tutorial_step];
    int hh = tutorial_highlight_h[setup_tutorial_step];
    int pulse = (tutorial_anim_frame / 5) % 2;
    if (pulse) {
        draw_rect(hx-2, hy-2, hw+4, hh+4, COLOR_DARK);
    } else {
        draw_rect(hx-1, hy-1, hw+2, hh+2, COLOR_LIGHT);
        draw_rect(hx, hy, hw, hh, COLOR_DARK);
    }
    y += 10;
    char step_str[32];
    snprintf(step_str, sizeof(step_str), "%d / %d", setup_tutorial_step+1, TUTORIAL_STEPS);
    draw_text_centered_in(0, y, screen_width, step_str, COLOR_MID, 1);
    if (setup_tutorial_step < TUTORIAL_STEPS - 1)
        setup_draw_buttons("Back", "Next");
    else
        setup_draw_buttons("Back", "Finish Tutorial");
    return 1;
}

static int setup_draw_step2(void) {
    setup_draw_topbar(0);
    int y = TOPBAR_H + 40;
    draw_text_centered_in(0, y, screen_width, "All Set!", COLOR_BLACK, 4);
    y += 60;
    draw_rounded_rect(60, y, screen_width-120, 2, 1, COLOR_LIGHT);
    y += 24;
    draw_text_centered_in(0, y, screen_width, "KindleJap is ready to use.", COLOR_DARK, 2);
    y += 34;
    draw_text_centered_in(0, y, screen_width, "You can always revisit the", COLOR_MID, 1);
    y += 18;
    draw_text_centered_in(0, y, screen_width, "tutorial by pressing the power", COLOR_MID, 1);
    y += 18;
    draw_text_centered_in(0, y, screen_width, "button and checking the menu.", COLOR_MID, 1);
    y += 50;
    draw_rounded_rect(80, y, screen_width-160, 100, 8, COLOR_LIGHT);
    y += 16;
    draw_text_centered_in(0, y, screen_width, "Created by", COLOR_MID, 1);
    y += 22;
    draw_text_centered_in(0, y, screen_width, "victorbillyph", COLOR_BLACK, 3);
    y += 38;
    draw_text_centered_in(0, y, screen_width, "github.com/victorbillyph", COLOR_DARK, 1);
    setup_draw_buttons(NULL, "Start KindleJap");
    return 1;
}

static void setup_draw(void) {
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
    switch (setup_step) {
        case 0: setup_draw_step0(); break;
        case 1: setup_draw_step1(); break;
        case 2: setup_draw_step2(); break;
    }
}

static void setup_handle(int tx, int ty, int released) {
    if (!released) return;
    int btn_h = 36;
    int btn_w = 160;
    int btn_y = screen_height - TOPBAR_H - btn_h - 20;

    if (setup_step == 0) {
        if (point_in_rect(tx, ty, screen_width - btn_w - 20, btn_y, btn_w, btn_h)) {
            setup_step = 1;
            setup_tutorial_step = 0;
            setup_init_tutorial();
            dirty = 1;
        }
    } else if (setup_step == 1) {
        int left_active = 1;
        int right_x = screen_width - btn_w - 20;
        if (point_in_rect(tx, ty, right_x, btn_y, btn_w, btn_h)) {
            if (setup_tutorial_step < TUTORIAL_STEPS - 1) {
                setup_tutorial_step++;
            } else {
                setup_step = 2;
            }
            tutorial_anim_frame = 0;
            dirty = 1;
        } else if (point_in_rect(tx, ty, 20, btn_y, btn_w, btn_h)) {
            if (setup_tutorial_step > 0) {
                setup_tutorial_step--;
            } else {
                setup_step = 0;
            }
            tutorial_anim_frame = 0;
            dirty = 1;
        }
    } else if (setup_step == 2) {
        if (point_in_rect(tx, ty, screen_width - btn_w - 20, btn_y, btn_w, btn_h)) {
            setup_finish();
        }
    }
}

#define MAX_INSTALLED_APPS 32
#define PKG_APPS_DIR "/mnt/us/extensions/kindlejap/apps"
#define PKG_INSTALLED_FILE "/mnt/us/extensions/kindlejap/data/installed.cfg"
#define PKG_MANIFEST_URL "https://raw.githubusercontent.com/%s/main/kindlejap-app.json"
#define PKG_MANIFEST_URL2 "https://raw.githubusercontent.com/%s/master/kindlejap-app.json"
#define PKG_RELEASE_URL "https://api.github.com/repos/%s/releases/latest"
#define PKG_MANIFEST_LOCAL "/tmp/kindlejap_pkg_manifest.json"
#define PKG_BINARY_LOCAL "/tmp/kindlejap_pkg_binary"

typedef struct {
    char name[64];
    char repo[128];
    char version[32];
    char description[128];
    int installed;
} PkgInfo;

static PkgInfo pkg_known[MAX_INSTALLED_APPS];
static int pkg_known_count = 0;
static int pkg_scroll = 0;
static int pkg_mode = 0;
#define PKG_MODE_VIEW 0
#define PKG_MODE_INPUT 1
#define PKG_MODE_INSTALLING 2
#define PKG_MODE_DONE 3
#define PKG_MODE_UNINSTALLING 4
static char pkg_status[256] = "";
static char pkg_input_buf[256] = "";
static int pkg_input_active = 0;

static void pkg_load_installed(void) {
    pkg_known_count = 0;
    const char *builtins[] = {"Calculator", "Files", "Network", "Browser", "Package Manager"};
    for (int i = 0; i < 5; i++) {
        strncpy(pkg_known[pkg_known_count].name, builtins[i], 63);
        pkg_known[pkg_known_count].repo[0] = 0;
        pkg_known[pkg_known_count].version[0] = 0;
        snprintf(pkg_known[pkg_known_count].description, sizeof(pkg_known[0].description), "Built-in app");
        pkg_known[pkg_known_count].installed = 1;
        pkg_known_count++;
    }
    FILE *f = fopen(PKG_INSTALLED_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && pkg_known_count < MAX_INSTALLED_APPS) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 0) continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *name = line, *repo = tab + 1;
        strncpy(pkg_known[pkg_known_count].name, name, 63);
        strncpy(pkg_known[pkg_known_count].repo, repo, 127);
        pkg_known[pkg_known_count].version[0] = 0;
        snprintf(pkg_known[pkg_known_count].description, sizeof(pkg_known[0].description), "External app");
        pkg_known[pkg_known_count].installed = 1;
        pkg_known_count++;
    }
    fclose(f);
}

static void pkg_save_installed(void) {
    FILE *f = fopen(PKG_INSTALLED_FILE, "w");
    if (!f) return;
    for (int i = 0; i < pkg_known_count; i++) {
        if (pkg_known[i].installed && pkg_known[i].repo[0])
            fprintf(f, "%s\t%s\n", pkg_known[i].name, pkg_known[i].repo);
    }
    fclose(f);
}

static int pkg_find_app(const char *name) {
    for (int i = 0; i < pkg_known_count; i++)
        if (strcmp(pkg_known[i].name, name) == 0) return i;
    return -1;
}

static int pkg_is_builtin(int idx) {
    return idx < 5;
}

static void pkg_install(const char *repo) {
    pkg_mode = PKG_MODE_INSTALLING;
    snprintf(pkg_status, sizeof(pkg_status), "Downloading manifest...");
    dirty = 1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -sL \"" PKG_MANIFEST_URL "\" -o " PKG_MANIFEST_LOCAL " 2>/dev/null", repo);
    system(cmd);
    FILE *f = fopen(PKG_MANIFEST_LOCAL, "r");
    if (!f) {
        snprintf(cmd, sizeof(cmd), "curl -sL \"" PKG_MANIFEST_URL2 "\" -o " PKG_MANIFEST_LOCAL " 2>/dev/null", repo);
        system(cmd);
        f = fopen(PKG_MANIFEST_LOCAL, "r");
    }
    if (!f) { snprintf(pkg_status, sizeof(pkg_status), "Failed: no manifest found"); pkg_mode = PKG_MODE_DONE; return; }

    char buf[2048]; int n = fread(buf, 1, sizeof(buf)-1, f);
    buf[n] = 0; fclose(f);

    char app_name[64] = "", app_binary[128] = "", app_version[32] = "", app_desc[128] = "";

    char *p = strstr(buf, "\"name\"");
    if (p) { p = strchr(p, ':'); if (p) { p++; while (*p==' '||*p=='"') p++; char *e = strchr(p, '"'); if (e) { int l=e-p; if(l<63) { strncpy(app_name, p, l); app_name[l]=0; } } } }
    p = strstr(buf, "\"binary\"");
    if (p) { p = strchr(p, ':'); if (p) { p++; while (*p==' '||*p=='"') p++; char *e = strchr(p, '"'); if (e) { int l=e-p; if(l<127) { strncpy(app_binary, p, l); app_binary[l]=0; } } } }
    p = strstr(buf, "\"version\"");
    if (p) { p = strchr(p, ':'); if (p) { p++; while (*p==' '||*p=='"') p++; char *e = strchr(p, '"'); if (e) { int l=e-p; if(l<31) { strncpy(app_version, p, l); app_version[l]=0; } } } }
    p = strstr(buf, "\"description\"");
    if (p) { p = strchr(p, ':'); if (p) { p++; while (*p==' '||*p=='"') p++; char *e = strchr(p, '"'); if (e) { int l=e-p; if(l<127) { strncpy(app_desc, p, l); app_desc[l]=0; } } } }

    if (strlen(app_name) == 0) { snprintf(pkg_status, sizeof(pkg_status), "Failed: no name in manifest"); pkg_mode = PKG_MODE_DONE; return; }
    if (strlen(app_binary) == 0) strcpy(app_binary, app_name);

    snprintf(pkg_status, sizeof(pkg_status), "Downloading %s...", app_name);
    dirty = 1;

    snprintf(cmd, sizeof(cmd), "curl -sL \"" PKG_RELEASE_URL "\" -o /tmp/kindlejap_pkg_release.json 2>/dev/null", repo);
    system(cmd);
    f = fopen("/tmp/kindlejap_pkg_release.json", "r");
    if (!f) { snprintf(pkg_status, sizeof(pkg_status), "Failed: can't fetch release"); pkg_mode = PKG_MODE_DONE; return; }
    n = fread(buf, 1, sizeof(buf)-1, f);
    buf[n] = 0; fclose(f);

    char dl_url[512] = "";
    p = strstr(buf, "\"browser_download_url\"");
    if (p) {
        p = strchr(p, ':');
        if (p) { p++; while (*p==' '||*p=='"') p++; char *e = strchr(p, '"'); if (e) { int l=e-p; if(l<511) { strncpy(dl_url, p, l); dl_url[l]=0; } } }
    }
    if (strlen(dl_url) == 0) { snprintf(pkg_status, sizeof(pkg_status), "Failed: no download URL"); pkg_mode = PKG_MODE_DONE; return; }

    char app_dir[256];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", PKG_APPS_DIR, app_name);
    mkdir(PKG_APPS_DIR, 0777);
    mkdir(app_dir, 0777);

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/%s-bin", app_dir, app_binary);
    snprintf(cmd, sizeof(cmd), "curl -sL \"%s\" -o \"%s\" 2>/dev/null", dl_url, bin_path);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "chmod +x \"%s\"", bin_path);
    system(cmd);

    snprintf(pkg_status, sizeof(pkg_status), "Installed %s v%s", app_name, strlen(app_version) ? app_version : "?");

    int idx = pkg_find_app(app_name);
    if (idx >= 0) {
        strncpy(pkg_known[idx].repo, repo, 127);
        strncpy(pkg_known[idx].version, app_version, 31);
        strncpy(pkg_known[idx].description, app_desc, 127);
        pkg_known[idx].installed = 1;
    } else if (pkg_known_count < MAX_INSTALLED_APPS) {
        strncpy(pkg_known[pkg_known_count].name, app_name, 63);
        strncpy(pkg_known[pkg_known_count].repo, repo, 127);
        strncpy(pkg_known[pkg_known_count].version, app_version, 31);
        strncpy(pkg_known[pkg_known_count].description, app_desc, 127);
        pkg_known[pkg_known_count].installed = 1;
        pkg_known_count++;
    }
    pkg_save_installed();
    pkg_mode = PKG_MODE_DONE;
}

static void pkg_uninstall(int idx) {
    if (idx < 0 || idx >= pkg_known_count) return;
    if (pkg_is_builtin(idx)) { snprintf(pkg_status, sizeof(pkg_status), "Can't uninstall built-in app"); pkg_mode = PKG_MODE_DONE; return; }
    pkg_mode = PKG_MODE_UNINSTALLING;
    snprintf(pkg_status, sizeof(pkg_status), "Uninstalling %s...", pkg_known[idx].name);
    dirty = 1;

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", PKG_APPS_DIR, pkg_known[idx].name);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", app_dir);
    system(cmd);

    pkg_known[idx].installed = 0;
    pkg_save_installed();

    snprintf(pkg_status, sizeof(pkg_status), "Uninstalled %s", pkg_known[idx].name);
    pkg_mode = PKG_MODE_DONE;
}

static void pkg_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);

    draw_rounded_rect(x+10, y+10, 140, 36, 8, COLOR_MID);
    draw_text_centered_in(x+10, y+18, 140, "Install", COLOR_WHITE, 2);

    if (strlen(pkg_status) > 0) {
        draw_rounded_rect(x+160, y+10, w-170, 36, 8, COLOR_LIGHT);
        draw_text(x+168, y+18, pkg_status, COLOR_DARK, 1);
    }

    int iy = y + 56;
    int visible = (h - 66) / 50;
    int total = 0;
    for (int i = 0; i < pkg_known_count; i++)
        if (pkg_known[i].installed) total++;

    if (pkg_scroll > total - visible) pkg_scroll = total - visible;
    if (pkg_scroll < 0) pkg_scroll = 0;

    int shown = 0;
    for (int i = 0; i < pkg_known_count && iy < y + h - 10; i++) {
        if (!pkg_known[i].installed) continue;
        if (shown < pkg_scroll) { shown++; continue; }
        shown++;

        draw_rounded_rect(x+10, iy, w-20, 44, 8, COLOR_LIGHT);
        draw_text(x+18, iy+6, pkg_known[i].name, COLOR_BLACK, 2);

        char sub[128];
        if (pkg_is_builtin(i)) {
            snprintf(sub, sizeof(sub), "Built-in");
            draw_text(x+18, iy+24, sub, COLOR_DARK, 1);
        } else {
            snprintf(sub, sizeof(sub), "%s", pkg_known[i].repo);
            draw_text(x+18, iy+24, sub, COLOR_DARK, 1);
            draw_rounded_rect(x+w-90, iy+6, 70, 32, 8, COLOR_MID);
            draw_text_centered_in(x+w-90, iy+12, 70, "Remove", COLOR_WHITE, 1);
        }
        iy += 50;
    }

    if (total == 0) {
        draw_text_centered_in(x, y+h/2, w, "No apps installed", COLOR_MID, 2);
    }
}

static void pkg_handle(int tx, int ty, int released) {
    if (!released) return;

    if (pkg_mode == PKG_MODE_DONE || pkg_mode == PKG_MODE_INSTALLING || pkg_mode == PKG_MODE_UNINSTALLING) {
        pkg_mode = PKG_MODE_VIEW;
        pkg_status[0] = 0;
        return;
    }

    if (point_in_rect(tx, ty, 10, TOPBAR_H+10, 140, 36)) {
        pkg_mode = PKG_MODE_INPUT;
        pkg_input_active = 1;
        pkg_input_buf[0] = 0;
        keyboard_visible = 1;
        keyboard_mode = 0;
        keyboard_cursor = 0;
        strcpy(keyboard_buf, "");
        return;
    }

    int iy = TOPBAR_H + 56;
    int visible = (screen_height - TOPBAR_H - 66) / 50;
    int shown = 0;
    for (int i = 0; i < pkg_known_count; i++) {
        if (!pkg_known[i].installed) continue;
        if (shown < pkg_scroll) { shown++; continue; }
        shown++;

        if (iy >= TOPBAR_H + 56 + visible * 50) break;

        if (!pkg_is_builtin(i) && point_in_rect(tx, ty, screen_width-80, iy+6, 70, 32)) {
            pkg_uninstall(i);
            return;
        }
        iy += 50;
    }
}

static void kual_scan_apps(void) {
    DIR *d = opendir("/mnt/us/extensions"); if (!d) return;
    struct dirent *e; int count = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    char msg[64]; snprintf(msg, sizeof(msg), "KUAL: %d extensions", count);
    log_msg(msg);
}

static void community_scan_apps(void) {
    DIR *d = opendir(COMMUNITY_APPS_DIR); if (!d) return;
    closedir(d);
    log_msg("Community apps scanned");
}

static void handle_signal(int sig) { running = 0; }
void handle_segfault(int sig) { log_msg("SEGFAULT"); restore_kindle_ui(); exit(1); }

static void show_splash(void) {
    draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
    draw_text_centered_in(0, screen_height/2 - 60, screen_width, "KindleJap", COLOR_BLACK, 5);
    draw_text_centered_in(0, screen_height/2 + 10, screen_width, KINDLEJAP_VERSION, COLOR_MID, 3);
    draw_text_centered_in(0, screen_height/2 + 60, screen_width, "Loading...", COLOR_MID, 2);
    refresh_screen();
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGSEGV, handle_segfault);
    signal(SIGBUS, handle_segfault);
    signal(SIGABRT, handle_segfault);
    log_msg("KindleJap v" KINDLEJAP_VERSION " starting...");
    if (acquire_lock() < 0) { log_msg("Another instance running"); return 1; }
    log_msg("Lock acquired");
    FILE *pf = popen("pidof lab126_gui", "r");
    if (pf) {
        if (fscanf(pf, "%d", &kindle_gui_pid) == 1 && kindle_gui_pid > 0) {
            char cmd[64]; snprintf(cmd, sizeof(cmd), "kill -STOP %d 2>/dev/null", kindle_gui_pid);
            system(cmd);
            log_msg("Paused Kindle UI");
        }
        pclose(pf);
    }
    if (kindle_gui_pid <= 0) {
        system("initctl stop lab126_gui 2>/dev/null");
        log_msg("Stopped Kindle UI (fallback)");
    }
    system("initctl stop otaupd 2>/dev/null");
    system("initctl stop phd 2>/dev/null");
    system("initctl stop tmd 2>/dev/null");
    system("initctl stop todo 2>/dev/null");
    system("initctl stop mcsd 2>/dev/null");
    log_msg("Hidden Kindle UI");
    system("lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null");
    init_framebuffer();
    init_input();
    init_power_button();
    data_init();
    data_load_settings();
    strncpy(file_path, settings.last_path, sizeof(file_path)-1);
    strncpy(browser_url, settings.browser_url, sizeof(browser_url)-1);
    file_scroll = settings.file_scroll;
    log_msg("Initialized");
    show_splash();
    sleep(1);
    kual_scan_apps();
    community_scan_apps();
    pkg_load_installed();
    notif_add("Welcome", "KindleJap v" KINDLEJAP_VERSION " ready", "Dismiss");
    if (!setup_is_done()) {
        setup_begin();
        log_msg("First run - starting setup");
    }
    int saved_app = data_load_appstate();
    if (saved_app >= 0 && saved_app < 5) {
        const char *names[] = {"Calculator", "Files", "Network", "Browser", "Package Manager"};
        for (int i = 0; i < open_count; i++) {
            if (strcmp(open_apps[i]->name, names[saved_app]) == 0) { active_app_idx = i; break; }
        }
    }
    log_msg("Apps loaded");
    int touch_x = 0, touch_y = 0;
    time_t last_save = 0;
    while (running) {
        if (input_fd >= 0) {
            struct input_event ev;
            while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_ABS) {
                    if (ev.code == 53) touch_x = ev.value;
                    else if (ev.code == 54) touch_y = ev.value;
                } else if (ev.type == EV_KEY && ev.code == 330) {
                    if (ev.value == 0) {
                        dirty = 1;
                        if (update_state == 1) { update_handle(touch_x, touch_y, 1); continue; }
                        if (setup_active) { setup_handle(touch_x, touch_y, 1); continue; }
                        if (keyboard_visible) {
                            keyboard_handle_touch(touch_x, touch_y);
                            if (pkg_input_active && !keyboard_visible) {
                                strncpy(pkg_input_buf, keyboard_buf, sizeof(pkg_input_buf)-1);
                                pkg_input_active = 0;
                                if (strlen(pkg_input_buf) > 0) pkg_install(pkg_input_buf);
                            }
                            if (browser_input_active && !keyboard_visible) {
                                strncpy(browser_url, keyboard_buf, sizeof(browser_url)-1);
                                browser_input_active = 0;
                                browser_load_url(browser_url);
                            }
                            continue;
                        }
                        if (downbar_visible) { downbar_handle_touch(touch_x, touch_y); continue; }
                        if (menu_visible) { menu_handle_touch(touch_x, touch_y); continue; }
                        if (notif_sidebar_visible) { notif_sidebar_handle(touch_x, touch_y); continue; }
                        if (touch_y < TOPBAR_H && touch_x >= screen_width - 180 && touch_x < screen_width - 140) {
                            notif_sidebar_visible = !notif_sidebar_visible; continue;
                        }
                        if (active_app_idx >= 0 && active_app_idx < open_count)
                            open_apps[active_app_idx]->on_touch(touch_x, touch_y, 1);
                    }
                }
            }
        }
        if (power_btn_fd >= 0) {
            struct input_event pev;
            while (read(power_btn_fd, &pev, sizeof(pev)) == sizeof(pev)) {
                if (pev.type == EV_KEY && pev.code == 116 && pev.value == 1) {
                    if (setup_active) continue;
                    dirty = 1;
                    if (menu_visible) { menu_visible = 0; }
                    else { downbar_visible = !downbar_visible; }
                }
            }
        }
        if (time(NULL) - last_save >= 30) {
            strncpy(settings.last_path, file_path, sizeof(settings.last_path)-1);
            strncpy(settings.browser_url, browser_url, sizeof(settings.browser_url)-1);
            settings.file_scroll = file_scroll;
            data_save_settings();
            last_save = time(NULL);
        }
        if (!dirty) { usleep(50000); continue; }
        draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
        if (setup_active) {
            setup_draw();
        } else {
            if (active_app_idx >= 0 && active_app_idx < open_count) {
                int app_h = screen_height - TOPBAR_H;
                open_apps[active_app_idx]->draw(0, TOPBAR_H, screen_width, app_h);
            }
            topbar_draw();
            downbar_draw();
            menu_draw();
            notif_sidebar_draw();
        }
        if (keyboard_visible) keyboard_draw();
        update_draw();
        refresh_screen();
        dirty = 0;
        usleep(50000);
    }
    for (int i=open_count-1; i>=0; i--)
        if (open_apps[i]->cleanup) open_apps[i]->cleanup();
    strncpy(settings.last_path, file_path, sizeof(settings.last_path)-1);
    strncpy(settings.browser_url, browser_url, sizeof(settings.browser_url)-1);
    settings.file_scroll = file_scroll;
    data_save_settings();
    data_save_appstate(active_app_idx);
    if (input_fd >= 0) close(input_fd);
    if (power_btn_fd >= 0) close(power_btn_fd);
    if (fb_mem && fb_mem != MAP_FAILED) munmap(fb_mem, finfo.smem_len);
    if (fb_fd >= 0) close(fb_fd);
    release_lock();
    restore_kindle_ui();
    log_msg("Exited cleanly");
    return 0;
}
