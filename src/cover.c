#include "cover.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <png.h>
#include <jpeglib.h>

static int conn_to(const char *host, int port)
{
    struct addrinfo hints, *res = NULL;
    char ps[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int read_line_fd(int fd, char *buf, size_t n)
{
    size_t i = 0;
    while (i + 1 < n) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (i && buf[i-1] == '\r') i--;
            buf[i] = 0;
            return (int)i;
        }
        buf[i++] = c;
    }
    buf[i] = 0;
    return (int)i;
}

static int read_all_fd(int fd, unsigned char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int fetch_art(mpd_t *m, const char *uri, unsigned char **out, size_t *outn)
{
    char line[512];
    int fd = conn_to(m->host, m->port);
    if (fd < 0) return -1;
    if (read_line_fd(fd, line, sizeof(line)) < 0) { close(fd); return -1; }

    char cmd[1200];
    /* quote URI and escape backslash/quote */
    char q[1024]; size_t j = 0;
    for (size_t i = 0; uri[i] && j + 2 < sizeof(q); i++) {
        if (uri[i] == '"' || uri[i] == '\\') q[j++] = '\\';
        q[j++] = uri[i];
    }
    q[j] = 0;
    snprintf(cmd, sizeof(cmd), "albumart \"%s\" 0\n", q);
    if (send(fd, cmd, strlen(cmd), MSG_NOSIGNAL) < 0) { close(fd); return -1; }

    size_t bsize = 0;
    for (;;) {
        if (read_line_fd(fd, line, sizeof(line)) < 0) { close(fd); return -1; }
        if (!strncmp(line, "ACK ", 4)) { close(fd); return -1; }
        if (!strncmp(line, "size: ", 6)) { continue; }
        if (!strncmp(line, "binary: ", 8)) { bsize = (size_t)strtoul(line + 8, NULL, 10); break; }
        if (!strcmp(line, "OK")) { close(fd); return -1; }
    }
    if (bsize == 0 || bsize > 20u*1024u*1024u) { close(fd); return -1; }
    unsigned char *buf = malloc(bsize);
    if (!buf) { close(fd); return -1; }
    if (read_all_fd(fd, buf, bsize) != 0) { free(buf); close(fd); return -1; }
    /* drain until OK */
    while (read_line_fd(fd, line, sizeof(line)) > 0) {
        if (!strcmp(line, "OK")) break;
    }
    close(fd);
    *out = buf; *outn = bsize;
    return 0;
}

static void scale_rgba_to565(const unsigned char *src, int sw, int sh, uint8_t *dst, int w, int h)
{
    for (int y = 0; y < h; y++) {
        int sy = y * sh / h;
        for (int x = 0; x < w; x++) {
            int sx = x * sw / w;
            const unsigned char *p = src + ((size_t)sy * sw + sx) * 4;
            uint16_t v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            dst[(size_t)(y * w + x) * 2] = (uint8_t)(v & 0xff);
            dst[(size_t)(y * w + x) * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
}

static int decode_png(const unsigned char *data, size_t n, uint8_t *dst, int w, int h)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, n)) return -1;
    img.format = PNG_FORMAT_RGBA;
    unsigned char *buf = malloc(PNG_IMAGE_SIZE(img));
    if (!buf) { png_image_free(&img); return -1; }
    int ok = png_image_finish_read(&img, NULL, buf, 0, NULL);
    if (ok) scale_rgba_to565(buf, (int)img.width, (int)img.height, dst, w, h);
    free(buf);
    png_image_free(&img);
    return ok ? 0 : -1;
}

static int decode_jpeg(const unsigned char *data, size_t n, uint8_t *dst, int w, int h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)n);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return -1; }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int sw = (int)cinfo.output_width, sh = (int)cinfo.output_height;
    if (sw <= 0 || sh <= 0) { jpeg_destroy_decompress(&cinfo); return -1; }
    size_t row = (size_t)sw * cinfo.output_components;
    unsigned char *buf = malloc(row * sh);
    if (!buf) { jpeg_destroy_decompress(&cinfo); return -1; }
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *p = buf + (size_t)cinfo.output_scanline * row;
        jpeg_read_scanlines(&cinfo, &p, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    for (int y = 0; y < h; y++) {
        int sy = y * sh / h;
        for (int x = 0; x < w; x++) {
            int sx = x * sw / w;
            const unsigned char *p = buf + ((size_t)sy * sw + sx) * 3;
            uint16_t v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            dst[(size_t)(y * w + x) * 2] = (uint8_t)(v & 0xff);
            dst[(size_t)(y * w + x) * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
    free(buf);
    return 0;
}

int cover_fetch_rgb565(mpd_t *m, const char *uri, uint8_t *dst, int w, int h)
{
    unsigned char *data = NULL; size_t n = 0;
    if (!uri || !*uri) return -1;
    if (fetch_art(m, uri, &data, &n) != 0) return -1;
    int rc = -1;
    if (n > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        rc = decode_png(data, n, dst, w, h);
    else if (n > 3 && data[0] == 0xff && data[1] == 0xd8)
        rc = decode_jpeg(data, n, dst, w, h);
    free(data);
    return rc;
}

/* --- file-based FLAC PICTURE extraction (bypasses MPD albumart) --- */
#include <stdint.h>

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int flac_picture(const char *path, unsigned char **out, size_t *outn)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "fLaC", 4) != 0) { fclose(f); return -1; }
    int last = 0;
    while (!last) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        last = bh[0] & 0x80;
        int type = bh[0] & 0x7f;
        uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
        if (type == 6) {
            unsigned char *blk = malloc(len);
            if (!blk) { fclose(f); return -1; }
            if (fread(blk, 1, len, f) != len) { free(blk); fclose(f); return -1; }
            uint32_t mlen = be32(blk + 4);
            if (8 + mlen + 4 > len) { free(blk); fclose(f); return -1; }
            uint32_t desc_len = be32(blk + 8 + mlen);
            size_t off_w = 12 + mlen + desc_len;
            if (off_w + 20 > len) { free(blk); fclose(f); return -1; }
            uint32_t dlen = be32(blk + off_w + 16);
            size_t off = off_w + 20;
            if (off + dlen > len) { free(blk); fclose(f); return -1; }
            unsigned char *img = malloc(dlen);
            if (!img) { free(blk); fclose(f); return -1; }
            memcpy(img, blk + off, dlen);
            free(blk); fclose(f);
            *out = img; *outn = dlen;
            return 0;
        }
        if (fseek(f, (long)len, SEEK_CUR) != 0) break;
    }
    fclose(f);
    return -1;
}

int cover_fetch_file_rgb565(const char *music_dir, const char *uri, uint8_t *dst, int w, int h)
{
    char path[1200];
    unsigned char *data = NULL; size_t n = 0;
    snprintf(path, sizeof(path), "%s/%s", music_dir, uri);
    if (flac_picture(path, &data, &n) != 0) return -1;
    int rc = -1;
    if (n > 8 && data[0] == 0x89 && data[1] == 'P') rc = decode_png(data, n, dst, w, h);
    else if (n > 3 && data[0] == 0xff && data[1] == 0xd8) rc = decode_jpeg(data, n, dst, w, h);
    free(data);
    return rc;
}

/* Cover iz foldera numere (cover.jpg / folder.jpg / front.* / prvi jpg|png). */
static int read_file(const char *path, unsigned char **out, size_t *outn)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return -1; }
    rewind(f);
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return -1; }
    *out = buf; *outn = got;
    return 0;
}

static int score_img(const char *name)
{
    if (!strcasecmp(name, "cover.jpg") || !strcasecmp(name, "cover.jpeg")) return 100;
    if (!strcasecmp(name, "folder.jpg") || !strcasecmp(name, "folder.jpeg")) return 90;
    if (!strcasecmp(name, "front.jpg") || !strcasecmp(name, "front.jpeg")) return 85;
    if (!strcasecmp(name, "cover.png")) return 80;
    if (!strcasecmp(name, "folder.png")) return 75;
    if (!strcasecmp(name, "front.png")) return 70;
    if (!strcasecmp(name, "album.jpg") || !strcasecmp(name, "albumart.jpg")) return 60;
    const char *d = strrchr(name, '.');
    if (d && (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".png"))) return 10;
    return 0;
}

int cover_fetch_dir_rgb565(const char *music_dir, const char *uri, uint8_t *dst, int w, int h)
{
    char path[1200], dir[1100];
    snprintf(path, sizeof(path), "%s/%s", music_dir, uri);
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (!sl) return -1;
    *sl = 0;

    DIR *d = opendir(dir);
    if (!d) return -1;

    char best[512] = "";
    int best_score = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        int sc = score_img(de->d_name);
        if (sc > best_score) {
            best_score = sc;
            snprintf(best, sizeof(best), "%s", de->d_name);
            if (sc >= 100) break;      /* cover.jpg je vec idealan */
        }
    }
    closedir(d);
    if (!best[0]) return -1;

    char img[1600];
    snprintf(img, sizeof(img), "%s/%s", dir, best);
    unsigned char *data = NULL; size_t n = 0;
    if (read_file(img, &data, &n) != 0) return -1;

    int rc = -1;
    if (n > 8 && data[0] == 0x89 && data[1] == 'P') rc = decode_png(data, n, dst, w, h);
    else if (n > 3 && data[0] == 0xff && data[1] == 0xd8) rc = decode_jpeg(data, n, dst, w, h);
    free(data);
    return rc;
}

