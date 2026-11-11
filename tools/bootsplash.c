/* bootsplash - nacrta 800x480 RGB565 sliku direktno u framebuffer.
 * Upotreba: bootsplash [/dev/fb0] [splash.raw]
 * Namerno bez zavisnosti (bez LVGL) da bi radio sto ranije u bootu.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define IMG_W 800
#define IMG_H 480

static unsigned char *read_file(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_n = (size_t)n;
    return buf;
}

static void log_uptime(const char *what)
{
    FILE *l = fopen("/run/oir-splash.log", "w");
    if (!l) return;
    char up[64] = "";
    FILE *u = fopen("/proc/uptime", "r");
    if (u) { if (!fgets(up, sizeof(up), u)) up[0] = 0; fclose(u); }
    fprintf(l, "%s at uptime %s", what, up);
    fclose(l);
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/fb0";
    const char *img = (argc > 2) ? argv[2] : "/opt/radio/splash/splash.raw";

    size_t n = 0;
    unsigned char *data = read_file(img, &n);
    if (!data) { log_uptime("splash image missing"); return 0; }
    if (n < (size_t)IMG_W * IMG_H * 2) { free(data); log_uptime("splash image short"); return 0; }

    int fd = open(dev, O_RDWR);
    if (fd < 0) { free(data); log_uptime("no framebuffer"); return 0; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        close(fd); free(data); log_uptime("fb ioctl failed"); return 0;
    }

    size_t fbsize = fi.smem_len ? fi.smem_len : (size_t)fi.line_length * vi.yres;
    unsigned char *fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { close(fd); free(data); log_uptime("mmap failed"); return 0; }

    int bpp = vi.bits_per_pixel;
    int px_per_line = (int)(fi.line_length / (bpp / 8));
    const uint16_t *src = (const uint16_t *)data;

    for (unsigned int y = 0; y < vi.yres; y++) {
        unsigned int sy = (y < IMG_H) ? y : IMG_H - 1;
        for (unsigned int x = 0; x < vi.xres; x++) {
            unsigned int sx = (x < IMG_W) ? x : IMG_W - 1;
            uint16_t v = src[(size_t)sy * IMG_W + sx];
            unsigned int ox = x + vi.xoffset, oy = y + vi.yoffset;
            size_t idx = (size_t)oy * px_per_line + ox;
            if (bpp == 16) {
                ((uint16_t *)fb)[idx] = v;
            } else if (bpp == 32) {
                unsigned int r = ((v >> 11) & 0x1f) << 3;
                unsigned int g = ((v >> 5) & 0x3f) << 2;
                unsigned int b = (v & 0x1f) << 3;
                ((uint32_t *)fb)[idx] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }
    }

    msync(fb, fbsize, MS_SYNC);
    munmap(fb, fbsize);
    close(fd);
    free(data);

    char what[128];
    snprintf(what, sizeof(what), "splash drawn %ux%u %dbpp", vi.xres, vi.yres, bpp);
    log_uptime(what);
    fprintf(stderr, "%s\n", what);
    return 0;
}
