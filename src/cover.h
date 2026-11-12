/* cover.h - fetch embedded album art from MPD and decode to RGB565 */
#ifndef COVER_H
#define COVER_H

#include <stdint.h>
#include <stddef.h>
#include "mpd_client.h"

/* returns 0 on success, fills dst (w*h) RGB565 little-endian */
int cover_fetch_rgb565(mpd_t *m, const char *uri, uint8_t *dst, int w, int h);

int cover_fetch_file_rgb565(const char *music_dir, const char *uri, uint8_t *dst, int w, int h);

/* fallback: slika iz foldera numere (cover.jpg/folder.jpg/front.*, prvi jpg/png) */
int cover_fetch_dir_rgb565(const char *music_dir, const char *uri, uint8_t *dst, int w, int h);

#endif
