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

#define KINDLEJAP_VERSION "2.1.0"
#define GITHUB_API_URL "https://api.github.com/repos/victorbillyph/kindlejap/releases/latest"
#define UPDATE_SCRIPT "/mnt/us/extensions/kindlejap/bin/update.sh"
#define LOCKFILE "/tmp/kindlejap.lock"
#define LOGFILE "/mnt/us/kindlejap.log"
#define COMMUNITY_APPS_DIR "/mnt/us/kindlejap_apps"
#define TASKBAR_H 52
#define KEYBOARD_H 290
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
    system("initctl start lab126_gui 2>/dev/null");
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

void draw_text_centered(int y, const char *t, unsigned char c, int s) {
    int w = (int)strlen(t) * FONT_W * s;
    draw_text((screen_width-w)/2, y, t, c, s);
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
static int keyboard_shift = 0;
static int keyboard_cursor = 0;
static char keyboard_buf[128] = "";

static const char *kb_rows[3] = {
    "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
};

void keyboard_draw(void) {
    int ky = screen_height - KEYBOARD_H - TASKBAR_H;
    draw_rounded_rect(0, ky, screen_width, KEYBOARD_H, CORNER_R, COLOR_LIGHTER);
    draw_rounded_rect(10, ky+10, screen_width-20, 38, 8, COLOR_WHITE);
    if (keyboard_cursor > 0) {
        draw_text(18, ky+18, keyboard_buf, COLOR_BLACK, 2);
        int tw = text_width(keyboard_buf, 2);
        if ((time(NULL)*2)%2==0) draw_rect(18+tw, ky+14, 2, 34, COLOR_BLACK);
    }
    int bw = (screen_width-60) / 10;
    for (int row=0; row<3; row++) {
        int len = strlen(kb_rows[row]);
        int off = (10-len) * bw / 2;
        for (int i=0; i<len; i++) {
            int bx = 30 + off + i*bw;
            int by = ky + 58 + row*58;
            char c = keyboard_shift ? kb_rows[row][i] : (kb_rows[row][i]+32);
            char label[2] = {c, 0};
            draw_rounded_rect(bx, by, bw-4, 52, 8, COLOR_WHITE);
            draw_text_centered(by+14, label, COLOR_BLACK, 2);
        }
    }
    int sx = screen_width - bw*2 - 30;
    int sy = ky + 58 + 2*58;
    draw_rounded_rect(sx, sy, bw*2-4, 52, 8, COLOR_DARK);
    draw_text_centered(sy+14, "SPACE", COLOR_WHITE, 2);
    draw_rounded_rect(20, sy, bw*2-4, 52, 8, COLOR_MID);
    char sh[2] = { keyboard_shift ? 'A' : 'a', 0 };
    draw_text_centered(sy+14, sh, COLOR_WHITE, 2);
    draw_rounded_rect(screen_width-bw*2-50, ky+58, bw*2-4, 52, 8, COLOR_DARK);
    draw_text_centered(ky+58+14, "BACK", COLOR_WHITE, 2);
    draw_rounded_rect(30, ky+14, 60, 30, 8, COLOR_MID);
    draw_text_centered(ky+18, "X", COLOR_WHITE, 2);
}

void keyboard_handle_touch(int tx, int ty) {
    int ky = screen_height - KEYBOARD_H - TASKBAR_H;
    if (point_in_rect(tx, ty, 30, ky+14, 60, 30)) {
        keyboard_visible = 0; return;
    }
    if (point_in_rect(tx, ty, 20, ky+58+2*58, (screen_width-60)/10*2-4, 52)) {
        keyboard_shift = !keyboard_shift; return;
    }
    int bw = (screen_width-60)/10;
    int sx = screen_width - bw*2 - 30;
    int sy = ky + 58 + 2*58;
    if (point_in_rect(tx, ty, sx, sy, bw*2-4, 52)) {
        if (keyboard_cursor < (int)sizeof(keyboard_buf)-1) {
            keyboard_buf[keyboard_cursor++] = ' ';
            keyboard_buf[keyboard_cursor] = 0;
        }
        return;
    }
    int bsx = screen_width-bw*2-50;
    if (point_in_rect(tx, ty, bsx, ky+58, bw*2-4, 52)) {
        if (keyboard_cursor > 0) { keyboard_cursor--; keyboard_buf[keyboard_cursor]=0; }
        return;
    }
    for (int row=0; row<3; row++) {
        int len = strlen(kb_rows[row]);
        int off = (10-len)*bw/2;
        for (int i=0; i<len; i++) {
            int bx = 30+off+i*bw;
            int by = ky+58+row*58;
            if (point_in_rect(tx, ty, bx, by, bw-4, 52)) {
                char c = keyboard_shift ? kb_rows[row][i] : (kb_rows[row][i]+32);
                if (keyboard_cursor < (int)sizeof(keyboard_buf)-1) {
                    keyboard_buf[keyboard_cursor++] = c;
                    keyboard_buf[keyboard_cursor] = 0;
                }
                return;
            }
        }
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
static void check_update(void);

static App calc_app = {"Calculator", NULL, calc_draw, calc_handle, NULL};
static App file_app = {"Files", NULL, file_draw, file_handle, NULL};
static App net_app = {"Network", net_init, net_draw, net_handle, NULL};
static App browser_app = {"Browser", NULL, browser_draw, browser_handle, NULL};

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
static int power_menu_visible = 0;

static int taskbar_power_btn_x(void) { return screen_width - 50; }

static void taskbar_draw(void) {
    int ty = screen_height - TASKBAR_H;
    draw_rect(0, ty, screen_width, TASKBAR_H, COLOR_DARK);
    int tx = 56;
    for (int i=0; i<open_count; i++) {
        int tw2 = text_width(open_apps[i]->name, 2) + 20;
        unsigned char bg = (i==active_app_idx) ? COLOR_MID : COLOR_DARK;
        draw_rounded_rect(tx, ty+8, tw2, 36, CORNER_R, bg);
        draw_text(tx+8, ty+12, open_apps[i]->name, COLOR_WHITE, 2);
        tx += tw2 + 8;
    }
    draw_rounded_rect(10, ty+8, 36, 36, CORNER_R, COLOR_WHITE);
    draw_rect(16, ty+18, 24, 2, COLOR_DARK);
    draw_rect(16, ty+24, 24, 2, COLOR_DARK);
    draw_rect(16, ty+30, 24, 2, COLOR_DARK);
    int px = taskbar_power_btn_x();
    draw_rounded_rect(px, ty+8, 36, 36, CORNER_R, COLOR_WHITE);
    int cy = ty + 26;
    draw_circle(px+18, cy, 7, COLOR_DARK);
    draw_rect(px+16, cy-10, 4, 5, COLOR_DARK);
}

static int taskbar_menu_btn(void) { return 10; }

static void menu_draw(void) {
    if (!menu_visible) return;
    int mw = 280, mh = 300;
    int mx = 10, my = screen_height - TASKBAR_H - mh - 10;
    draw_rounded_rect(mx, my, mw, mh, CORNER_R, COLOR_WHITE);
    const char *items[] = {"Calculator", "Files", "Network", "Browser", "Check Update", "Exit"};
    for (int i=0; i<6; i++) {
        int iy = my + 10 + i*46;
        draw_rounded_rect(mx+10, iy, mw-20, 40, 8, COLOR_LIGHT);
        draw_text(mx+20, iy+10, items[i], COLOR_BLACK, 2);
    }
}

static void menu_handle_touch(int tx, int ty) {
    int mw=280, mh=300;
    int mx=10, my=screen_height-TASKBAR_H-mh-10;
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
        menu_visible=0;
        check_update();
        return;
    }
    int iy5 = my+10+5*46;
    if (point_in_rect(tx, ty, mx+10, iy5, mw-20, 40)) {
        running=0; menu_visible=0;
    }
}

static void power_menu_draw(void) {
    if (!power_menu_visible) return;
    int pw=200, ph=140;
    int px=taskbar_power_btn_x()-pw+36, py=screen_height-TASKBAR_H-ph-10;
    draw_rounded_rect(px, py, pw, ph, CORNER_R, COLOR_WHITE);
    draw_rounded_rect(px+10, py+10, pw-20, 38, 8, COLOR_LIGHT);
    draw_text(px+20, py+18, "Sleep", COLOR_BLACK, 2);
    draw_rounded_rect(px+10, py+56, pw-20, 38, 8, COLOR_LIGHT);
    draw_text(px+20, py+64, "Power Off", COLOR_BLACK, 2);
}

static void power_menu_handle_touch(int tx, int ty) {
    int pw=200, ph=140;
    int px=taskbar_power_btn_x()-pw+36, py=screen_height-TASKBAR_H-ph-10;
    if (!point_in_rect(tx, ty, px, py, pw, ph)) { power_menu_visible=0; return; }
    if (point_in_rect(tx, ty, px+10, py+10, pw-20, 38)) {
        power_menu_visible=0;
        system("lipc-set-prop com.lab126.powerd sleep 1 2>/dev/null");
        return;
    }
    if (point_in_rect(tx, ty, px+10, py+56, pw-20, 38)) {
        power_menu_visible=0; running=0;
    }
}

static int menu_toggle(void) { menu_visible = !menu_visible; power_menu_visible=0; return menu_visible; }
static int power_menu_toggle(void) { power_menu_visible = !power_menu_visible; menu_visible=0; return power_menu_visible; }

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
    draw_text_centered(dy+132, "Install", COLOR_WHITE, 2);
    draw_rounded_rect(dx+210, dy+120, 170, 50, 8, COLOR_LIGHT);
    draw_text_centered(dy+132, "Cancel", COLOR_DARK, 2);
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
            draw_text_centered(ky+18, keys[r*4+c], COLOR_BLACK, 3);
        }
}

static void calc_handle(int tx, int ty, int released) {
    if (!released) return;
    int bw=(screen_width-40)/4, bh=60;
    for (int r=0; r<4; r++)
        for (int c=0; c<4; c++) {
            int kx=10+c*bw, ky=80+r*bh;
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
    int iy = 56;
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
    if (ty < 46) {
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
    int iy = 60;
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

static void browser_draw(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, COLOR_WHITE);
    draw_rounded_rect(x+10, y+10, w-20, 36, 8, COLOR_LIGHT);
    draw_text(x+18, y+16, browser_url, COLOR_BLACK, 2);
    if (browser_input_active) {
        draw_text_centered(y+80, "Type URL and press Enter", COLOR_MID, 2);
    } else {
        draw_text_centered(y+80, "Tap URL bar to type", COLOR_MID, 2);
    }
}

static void browser_handle(int tx, int ty, int released) {
    if (!released) return;
    if (point_in_rect(tx, ty, 10, 10, screen_width-20, 36)) {
        keyboard_visible = 1;
        keyboard_cursor = strlen(browser_url);
        strcpy(keyboard_buf, browser_url);
        browser_input_active = 1;
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
    draw_text_centered(screen_height/2 - 60, "KindleJap", COLOR_BLACK, 5);
    draw_text_centered(screen_height/2 + 10, KINDLEJAP_VERSION, COLOR_MID, 3);
    draw_text_centered(screen_height/2 + 60, "Loading...", COLOR_MID, 2);
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
    system("initctl stop lab126_gui 2>/dev/null");
    system("initctl stop otaupd 2>/dev/null");
    system("initctl stop phd 2>/dev/null");
    system("initctl stop tmd 2>/dev/null");
    system("initctl stop todo 2>/dev/null");
    system("initctl stop mcsd 2>/dev/null");
    log_msg("Hidden Kindle UI");
    system("lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null");
    init_framebuffer();
    init_input();
    log_msg("Initialized");
    show_splash();
    sleep(1);
    kual_scan_apps();
    community_scan_apps();
    log_msg("Apps loaded");
    int touch_x = 0, touch_y = 0;
    while (running) {
        draw_rect(0, 0, screen_width, screen_height, COLOR_WHITE);
        if (active_app_idx >= 0 && active_app_idx < open_count) {
            int app_h = screen_height - TASKBAR_H;
            open_apps[active_app_idx]->draw(0, 0, screen_width, app_h);
        }
        taskbar_draw();
        menu_draw();
        power_menu_draw();
        if (keyboard_visible) keyboard_draw();
        update_draw();
        refresh_screen();
        if (input_fd >= 0) {
            struct input_event ev;
            while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_ABS) {
                    if (ev.code == 53) touch_x = ev.value;
                    else if (ev.code == 54) touch_y = ev.value;
                } else if (ev.type == EV_KEY && ev.code == 330) {
                    if (ev.value == 0) {
                        if (update_state == 1) { update_handle(touch_x, touch_y, 1); continue; }
                        if (keyboard_visible) {
                            keyboard_handle_touch(touch_x, touch_y);
                            if (browser_input_active && !keyboard_visible) {
                                strncpy(browser_url, keyboard_buf, sizeof(browser_url)-1);
                                browser_input_active = 0;
                            }
                            continue;
                        }
                        if (touch_y >= screen_height - TASKBAR_H) {
                            if (point_in_rect(touch_x, touch_y, taskbar_menu_btn(), screen_height-TASKBAR_H+8, 36, 36))
                                menu_toggle();
                            else if (point_in_rect(touch_x, touch_y, taskbar_power_btn_x(), screen_height-TASKBAR_H+8, 36, 36))
                                power_menu_toggle();
                            else {
                                int tw = 56;
                                for (int i=0; i<open_count; i++) {
                                    int ww = text_width(open_apps[i]->name, 2) + 20;
                                    if (touch_x >= tw-2 && touch_x < tw+ww+2) active_app_idx = i;
                                    tw += ww + 8;
                                }
                            }
                            continue;
                        }
                        if (menu_visible) { menu_handle_touch(touch_x, touch_y); continue; }
                        if (power_menu_visible) { power_menu_handle_touch(touch_x, touch_y); continue; }
                        if (active_app_idx >= 0 && active_app_idx < open_count)
                            open_apps[active_app_idx]->on_touch(touch_x, touch_y, 1);
                    }
                }
            }
        }
        usleep(30000);
    }
    for (int i=open_count-1; i>=0; i--)
        if (open_apps[i]->cleanup) open_apps[i]->cleanup();
    if (input_fd >= 0) close(input_fd);
    if (fb_mem && fb_mem != MAP_FAILED) munmap(fb_mem, finfo.smem_len);
    if (fb_fd >= 0) close(fb_fd);
    release_lock();
    restore_kindle_ui();
    log_msg("Exited cleanly");
    return 0;
}
