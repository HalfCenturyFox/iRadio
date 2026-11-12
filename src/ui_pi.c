/* ui_pi.c - OIR UI 800x480 (LVGL 9, fbdev+evdev) */
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "mpd_client.h"
#include "stations.h"
#include "cover.h"
#include "icons.h"
#include "icons_jack.h"

#define COL_BG      0x121212
#define COL_PANEL   0x1E1E1E
#define COL_TEXT    0xE0E0E0
#define COL_DIM     0x9E9E9E
#define COL_ACCENT  0x26A69A
#define COL_BORDER  0x333333

#define COVER_X 24
#define COVER_Y 24
#define COVER_W 320
#define COVER_H 320

static mpd_t *g_mpd;
static int g_connected;
static lv_obj_t *scr_player, *scr_stations, *scr_settings, *scr_local, *scr_menu, *scr_queue;
static lv_obj_t *queue_list;
static lv_obj_t *scr_pl, *pl_list;
#define PL_MAX 64
static char pl_names[PL_MAX][128];
static int pl_sel[PL_MAX];
static int pl_count;
static lv_obj_t *pl_btns[PL_MAX];
static lv_obj_t *lbl_top, *lbl_title, *lbl_sub, *lbl_time, *lbl_vol;
static lv_obj_t *lbl_album, *lbl_src, *lbl_data, *lbl_elapsed, *lbl_total, *lbl_clock;
static lv_obj_t *btn_out_icon, *lbl_out_icon, *img_out_icon, *img_menu_icon;
static lv_obj_t *slider_vol_menu, *lbl_vol_menu;
static lv_obj_t *bar_prog;
static lv_obj_t *slider_vol;
static lv_obj_t *img_play;
static lv_obj_t *btn_out_lbl;
static lv_obj_t *btn_out;
static lv_obj_t *station_btns[64];
static lv_obj_t *stations_list;
static int active_station = -1;
static int last_station = -1;
static int last_saved = -1;
#define LAST_STATION_FILE "/opt/radio/last-station"
#define NAV_MAX 8
static lv_obj_t *nav_stack[NAV_MAX];
static int nav_depth;

/* napred: zapamti trenutni ekran i predji na novi */
static void nav_to(lv_obj_t *scr)
{
    lv_obj_t *cur = lv_screen_active();
    if (cur && cur != scr && nav_depth < NAV_MAX) nav_stack[nav_depth++] = cur;
    lv_screen_load(scr);
}

/* nazad: jedan ekran gore */
static void nav_back(void)
{
    if (nav_depth > 0) lv_screen_load(nav_stack[--nav_depth]);
    else lv_screen_load(scr_player);
}

/* na player iz akcije (pusti stanicu/num eru) - resetuje istoriju */
static void nav_home(void)
{
    nav_depth = 0;
    lv_screen_load(scr_player);
}

static void stations_refresh(void);
static void stations_center_active(void);
static void stations_center_cb(lv_timer_t *t);
static lv_obj_t *canvas_cover;
static uint8_t cover_buf[COVER_W * COVER_H * 2];
static int vol_drag;
static int bt_mode;
static char cur_url[512];
static char cur_art_url[512];
static char cur_logo[128];

static void slugify(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) out[j++] = (char)tolower(c);
        else if (j && out[j-1] != '-') out[j++] = '-';
    }
    while (j && out[j-1] == '-') j--;
    out[j] = 0;
}

static void cover_clear(void)
{
    for (size_t i = 0; i + 1 < sizeof(cover_buf); i += 2) { cover_buf[i] = 0xC3; cover_buf[i+1] = 0x18; }
}

static int load_logo(const char *name)
{
    char slug[128], path[256];
    slugify(name, slug, sizeof(slug));
    snprintf(path, sizeof(path), "/opt/radio/assets/logos565/%s.rgb565", slug);
    FILE *f = fopen(path, "rb");
    if (!f) {
        cover_clear();
        return -1;
    }
    size_t n = fread(cover_buf, 1, sizeof(cover_buf), f);
    fclose(f);
    if (n < sizeof(cover_buf)) memset(cover_buf + n, 0x18, sizeof(cover_buf) - n);
    return 0;
}


static void set_cover_station(const char *name);

/* Grupni prefiks za prikaz: "SomaFM Lush" -> "SomaFM - Lush" */
static const char *group_prefixes[] = { "SomaFM", "Naxi", NULL };

static void station_display(const char *url, const char *icy, char *out, size_t n)
{
    for (int i = 0; i < STATION_COUNT; i++) {
        size_t k = strlen(stations[i].url);
        if (k && !strncmp(url, stations[i].url, k)) {
            const char *name = stations[i].name;
            for (int g = 0; group_prefixes[g]; g++) {
                size_t gl = strlen(group_prefixes[g]);
                if (!strncmp(name, group_prefixes[g], gl) && name[gl] == ' ') {
                    snprintf(out, n, "%s - %s", group_prefixes[g], name + gl + 1);
                    return;
                }
            }
            snprintf(out, n, "%s", name);
            return;
        }
    }
    snprintf(out, n, "%s", (icy && *icy) ? icy : "RADIO");
}

/* Skraceni tekst poruke streama (kad nema podataka o pesmi) */
static void shorten_text(const char *in, char *out, size_t n, size_t max_chars)
{
    if (!in) { out[0] = 0; return; }
    size_t len = strlen(in);
    while (len && (in[len-1] == ' ' || in[len-1] == '\n' || in[len-1] == '\r')) len--;
    if (len <= max_chars) { snprintf(out, n, "%.*s", (int)len, in); return; }
    size_t cut = max_chars > 3 ? max_chars - 3 : max_chars;
    snprintf(out, n, "%.*s...", (int)cut, in);
}

/* "Artist - Album" -> artist + album (SPLIT na prvi " - ") */
static void split_artist_album(const char *in, char *artist, size_t an,
                               char *album, size_t bn)
{
    artist[0] = album[0] = 0;
    if (!in || !*in) return;
    const char *sep = strstr(in, " - ");
    if (!sep) { snprintf(artist, an, "%s", in); return; }
    snprintf(artist, an, "%.*s", (int)(sep - in), in);
    snprintf(album, bn, "%s", sep + 3);
}

/* Kodek iz imena/URL-a fajla: FLAC, MP3, AAC, OGG... */
static const char *codec_for_url(const char *url)
{
    const char *dot = strrchr(url ? url : "", '.');
    if (!dot || !dot[1]) return "STREAM";
    const char *e = dot + 1;
    if (!strcasecmp(e, "flac")) return "FLAC";
    if (!strcasecmp(e, "mp3"))  return "MP3";
    if (!strcasecmp(e, "m4a") || !strcasecmp(e, "aac") || !strcasecmp(e, "mp4")) return "AAC";
    if (!strcasecmp(e, "ogg") || !strcasecmp(e, "opus")) return "OGG";
    if (!strcasecmp(e, "wav"))  return "WAV";
    if (!strcasecmp(e, "m3u8")) return "HLS";
    return "STREAM";
}

/* Red sa formatom: "FLAC | 44.1 kHz | 24 bit | 2 ch" ili "MP3 | 320 kbps | 44.1 kHz" */
static void fmt_audio_line(const mpd_status_t *st, const char *codec,
                           char *out, size_t n)
{
    char sr[32] = "";
    if (st && st->audio_rate > 0)
        snprintf(sr, sizeof(sr), "%.1f kHz", st->audio_rate / 1000.0);
    if (st && st->bitrate > 0 && st->audio_bits <= 0) {
        snprintf(out, n, "%s | %d kbps | %s", codec, st->bitrate, sr);
    } else if (st && st->audio_bits > 0) {
        snprintf(out, n, "%s | %s | %d bit | %d ch", codec, sr,
                 st->audio_bits, st->audio_ch);
    } else if (sr[0]) {
        snprintf(out, n, "%s | %s", codec, sr);
    } else {
        snprintf(out, n, "%s", codec);
    }
}

static void fmt_mmss(int sec, char *out, size_t n)
{
    if (sec < 0) { out[0] = 0; return; }
    if (sec >= 3600)
        snprintf(out, n, "%d:%02d:%02d", sec/3600, (sec%3600)/60, sec%60);
    else
        snprintf(out, n, "%d:%02d", sec/60, sec%60);
}


/* pozadinska slika: 800x480 RGB565, konvertovana iz .avif (LVGL ne dekodira AVIF) */
#define BG_W 800
#define BG_H 480
#define BG_PATH "/opt/radio/assets/bg800.rgb565"
#define BG_OPA 140
#define MPD_BUF_Q 32768
static uint16_t bg_buf[BG_W * BG_H];
static uint16_t splash_buf[BG_W * BG_H];
static lv_image_dsc_t splash_dsc;
static int splash_ready = 0;
static lv_obj_t *scr_splash;
static lv_image_dsc_t bg_dsc;
static int bg_ready = 0;

static void bg_init(void)
{
    FILE *f = fopen(BG_PATH, "rb");
    if (!f) return;
    size_t n = fread(bg_buf, 1, sizeof(bg_buf), f);
    fclose(f);
    if (n != sizeof(bg_buf)) return;
    memset(&bg_dsc, 0, sizeof(bg_dsc));
    bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    bg_dsc.header.w = BG_W;
    bg_dsc.header.h = BG_H;
    bg_dsc.header.stride = BG_W * 2;
    bg_dsc.data = (const uint8_t *)bg_buf;
    bg_dsc.data_size = sizeof(bg_buf);
    bg_ready = 1;
}

#define SPLASH_PATH "/opt/radio/assets/splash800.rgb565"

static void splash_init(void)
{
    FILE *f = fopen(SPLASH_PATH, "rb");
    if (!f) return;
    size_t n = fread(splash_buf, 1, sizeof(splash_buf), f);
    fclose(f);
    if (n != sizeof(splash_buf)) return;
    memset(&splash_dsc, 0, sizeof(splash_dsc));
    splash_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    splash_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    splash_dsc.header.w = BG_W;
    splash_dsc.header.h = BG_H;
    splash_dsc.header.stride = BG_W * 2;
    splash_dsc.data = (const uint8_t *)splash_buf;
    splash_dsc.data_size = sizeof(splash_buf);
    splash_ready = 1;
}

static void apply_bg(lv_obj_t *scr)
{
    if (!bg_ready || !scr) return;
    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &bg_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_style_opa(img, BG_OPA, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(img);
}

static void last_station_load(void)
{
    FILE *f = fopen(LAST_STATION_FILE, "r");
    if (!f) return;
    int v = -1;
    if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < STATION_COUNT) last_station = v;
    fclose(f);
}

static void last_station_save(int idx)
{
    if (idx < 0 || idx >= STATION_COUNT || idx == last_saved) return;
    last_saved = idx;
    FILE *f = fopen(LAST_STATION_FILE, "w");
    if (!f) return;
    fprintf(f, "%d\n", idx);
    fclose(f);
}

static void set_cover_for_url(const char *url, const char *fallback)
{
    for (int i = 0; i < STATION_COUNT; i++) {
        size_t n = strlen(stations[i].url);
        if (n && !strncmp(url, stations[i].url, n)) {
            set_cover_station(stations[i].name);
            return;
        }
    }
    if (fallback && *fallback) set_cover_station(fallback);
}

static void set_cover_station(const char *name)
{
    if (!strcmp(name, cur_logo)) return;
    snprintf(cur_logo, sizeof(cur_logo), "%s", name);
    load_logo(name);
    lv_obj_invalidate(canvas_cover);
}

static void style_btn(lv_obj_t *b, uint32_t bg, uint32_t fg, int accent)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, 255, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, accent ? 2 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent ? COL_ACCENT : COL_BORDER), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(fg), 0);
    /* disabled stanje: ista tamna pozadina, prigasen tekst (bez svetle teme) */
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(b, 255, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_BORDER), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_DIM), LV_STATE_DISABLED);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, int x, int y, int w, int h, const char *txt, lv_event_cb_t cb, void *ud, int accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    style_btn(b, accent ? COL_ACCENT : COL_PANEL, accent ? 0x0E0E0E : COL_TEXT, accent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}


static lv_obj_t *mk_round_btn(lv_obj_t *parent, int x, int y, int d, const char *txt,
                              lv_event_cb_t cb, void *ud, int accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, d, d);
    style_btn(b, accent ? COL_ACCENT : COL_PANEL, accent ? 0x0E0E0E : COL_TEXT, accent);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}


static lv_obj_t *mk_round_icon_btn(lv_obj_t *parent, int x, int y, int d,
                                   const lv_image_dsc_t *icon, uint32_t tint,
                                   lv_event_cb_t cb, void *ud, int accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, d, d);
    style_btn(b, accent ? COL_ACCENT : COL_PANEL, accent ? 0x0E0E0E : COL_TEXT, accent);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *img = lv_image_create(b);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(tint), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* ---------- LOKAL: fajlovi sa Pi-a (/opt/radio/music) ---------- */
#define LOCAL_MAX 200
#define LOCAL_NLEN 160
#define LOCAL_ROOT "/mnt/nas"

static int local_sel[LOCAL_MAX];
static lv_obj_t *local_btns[LOCAL_MAX];

static char local_path[512];
static char local_names[LOCAL_MAX][LOCAL_NLEN];
static int local_isdir[LOCAL_MAX];
static int local_count;
static lv_obj_t *local_list;
static lv_obj_t *local_path_lbl;
static int local_songs;
static void local_item_event(lv_event_t *e);

static void local_swap(int a, int b)
{
    char t[LOCAL_NLEN];
    snprintf(t, sizeof(t), "%s", local_names[a]);
    snprintf(local_names[a], LOCAL_NLEN, "%s", local_names[b]);
    snprintf(local_names[b], LOCAL_NLEN, "%s", t);
    int d = local_isdir[a]; local_isdir[a] = local_isdir[b]; local_isdir[b] = d;
}

static void local_scan(void)
{
    local_count = 0;
    local_songs = 0;
    DIR *d = opendir(local_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && local_count < LOCAL_MAX) {
        if (de->d_name[0] == '.') continue;
        char full[768];
        snprintf(full, sizeof(full), "%s/%s", local_path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        int isdir = S_ISDIR(st.st_mode) ? 1 : 0;
        if (!isdir) {
            const char *dot = strrchr(de->d_name, '.');
            if (!dot) continue;
            static const char *ok[] = { ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".wav", ".opus",
                                    ".wv", ".dsf", ".dff", ".aiff", ".aif", NULL };
            int good = 0;
            for (int i = 0; ok[i]; i++) if (!strcasecmp(dot, ok[i])) { good = 1; break; }
            if (!good) continue;
        }
        snprintf(local_names[local_count], LOCAL_NLEN, "%s", de->d_name);
        local_isdir[local_count] = isdir;
        if (!isdir) local_songs++;
        local_count++;
    }
    closedir(d);
    for (int i = 0; i < local_count; i++)
        for (int j = i + 1; j < local_count; j++) {
            int sw = 0;
            if (local_isdir[i] != local_isdir[j]) sw = local_isdir[j] > local_isdir[i];
            else if (strcasecmp(local_names[i], local_names[j]) > 0) sw = 1;
            if (sw) local_swap(i, j);
        }
}

static void local_fill(void)
{
    if (!local_list) return;
    for (int k = 0; k < LOCAL_MAX; k++) local_btns[k] = NULL;
    if (local_path_lbl) {
        const char *rp = local_path + strlen(LOCAL_ROOT);
        while (*rp == '/') rp++;
        char pretty[300];
        snprintf(pretty, sizeof(pretty), "NAS / %s", *rp ? rp : "");
        lv_label_set_text(local_path_lbl, pretty);
    }
    lv_obj_clean(local_list);
    if (local_count == 0) {
        lv_obj_t *l = lv_label_create(local_list);
        lv_label_set_text(l, "No music on NAS");
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        return;
    }
    for (int i = 0; i < local_count; i++) {
        lv_obj_t *b = lv_button_create(local_list);
        lv_obj_set_size(b, 760, 56);
        int on = (!local_isdir[i] && local_sel[i]);
        style_btn(b, on ? 0x3B3117 : COL_PANEL, on ? 0xF2CB61 : COL_TEXT, on ? 1 : 0);
        if (on) lv_obj_set_style_border_color(b, lv_color_hex(0xD4A72C), 0);
        lv_obj_t *l = lv_label_create(b);
        char txt[LOCAL_NLEN + 8];
        if (local_isdir[i]) snprintf(txt, sizeof(txt), "[%s]", local_names[i]);
        else snprintf(txt, sizeof(txt), "%s", local_names[i]);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_add_event_cb(b, local_item_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        local_btns[i] = b;
    }
}


static void local_rel_path(int idx, char *out, size_t n)
{
    char full[900];
    snprintf(full, sizeof(full), "%s/%s", local_path, local_names[idx]);
    const char *rp = full;
    size_t rl = strlen(LOCAL_ROOT);
    if (!strncmp(rp, LOCAL_ROOT, rl)) {
        rp += rl;
        while (*rp == '/') rp++;
    }
    snprintf(out, n, "%s", rp);
}

static void local_mpd_update_dir(void)
{
    char rel[600], cmd[700];
    const char *rp = local_path + strlen(LOCAL_ROOT);
    while (*rp == '/') rp++;
    snprintf(rel, sizeof(rel), "%s", rp);
    snprintf(cmd, sizeof(cmd), "update \"%s\"", rel);
    mpd_cmd(g_mpd, cmd);
}

static void local_queue(int start_idx)
{
    char rel[600], cmd[700];
    int pos = 0, start_pos = 0;

    /* novi fajlovi sa NAS-a nisu u MPD bazi dok se ne skeniraju */
    local_mpd_update_dir();
    mpd_cmd(g_mpd, "clear");

    for (int i = 0; i < local_count; i++) {
        if (local_isdir[i]) continue;
        local_rel_path(i, rel, sizeof(rel));
        snprintf(cmd, sizeof(cmd), "add \"%s\"", rel);
        int rc = mpd_cmd(g_mpd, cmd);
        for (int t = 0; t < 6 && rc != 0; t++) {   /* sacekaj skeniranje baze */
            usleep(700000);
            rc = mpd_cmd(g_mpd, cmd);
        }
        if (i == start_idx) start_pos = pos;
        pos++;
    }
    if (pos == 0) return;
    /* da MPD sam prelazi na sledecu numeru (MPD trazi 0/1, ne off) */
    mpd_cmd(g_mpd, "single 0");
    mpd_cmd(g_mpd, "repeat 0");
    snprintf(cmd, sizeof(cmd), "play %d", start_pos);
    mpd_cmd(g_mpd, cmd);
    snprintf(cur_logo, sizeof(cur_logo), "%s", "");
    nav_home();
}

static void local_play_all(void)
{
    if (!g_connected) return;
    int first = -1;
    for (int i = 0; i < local_count; i++) {
        if (!local_isdir[i]) { first = i; break; }
    }
    if (first < 0) return;
    local_queue(first);
}

static void local_all_event(lv_event_t *e) { (void)e; local_play_all(); }

static void local_item_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= local_count) return;
    if (local_isdir[idx]) {
        for (int k = 0; k < LOCAL_MAX; k++) local_sel[k] = 0;
        char np[512];
        snprintf(np, sizeof(np), "%s/%s", local_path, local_names[idx]);
        snprintf(local_path, sizeof(local_path), "%s", np);
        local_scan();
        local_fill();
        if (g_connected) local_mpd_update_dir();
        return;
    }
    /* fajl: tap = selektuj/deselektuj (multi-select, oker) */
    local_sel[idx] = !local_sel[idx];
    lv_obj_t *b = local_btns[idx];
    if (b) {
        int on = local_sel[idx];
        style_btn(b, on ? 0x3B3117 : COL_PANEL, on ? 0xF2CB61 : COL_TEXT, on ? 1 : 0);
        if (on) lv_obj_set_style_border_color(b, lv_color_hex(0xD4A72C), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? 0xF2CB61 : COL_TEXT), 0);
    }
}

static void local_up_event(lv_event_t *e)
{
    for (int k = 0; k < LOCAL_MAX; k++) local_sel[k] = 0;
    (void)e;
    size_t rl = strlen(LOCAL_ROOT);
    if (strlen(local_path) <= rl) return;
    char *sl = strrchr(local_path, '/');
    if (!sl || (size_t)(sl - local_path) < rl) return;
    *sl = '\0';
    local_scan();
    local_fill();
}

static void power_event(lv_event_t *e)
{
    (void)e;
    sync();
    system("systemctl poweroff");
}

static void queue_event(lv_event_t *e);
static void pl_fill(void);
static void pl_event(lv_event_t *e);
static void queue_fill(void);

static void menu_event(lv_event_t *e) { (void)e; nav_to(scr_menu); }
static void back_event(lv_event_t *e) { (void)e; nav_back(); }
static void stations_event(lv_event_t *e)
{
    (void)e;
    stations_refresh();
    nav_to(scr_stations);
    lv_timer_create(stations_center_cb, 150, NULL);
}
static void settings_event(lv_event_t *e) { (void)e; nav_to(scr_settings); }
static void local_event(lv_event_t *e)
{
    (void)e;
    local_scan();
    local_fill();
    if (g_connected) local_mpd_update_dir();
    nav_to(scr_local);
}

static void prev_event(lv_event_t *e) { (void)e; mpd_previous(g_mpd); }
static void next_event(lv_event_t *e) { (void)e; mpd_next(g_mpd); }
static void play_event(lv_event_t *e)
{
    (void)e;
    if (!g_connected) return;
    mpd_toggle(g_mpd);
}


static int mpd_output_enabled(int id)
{
    char buf[1024];
    if (mpd_cmd_read(g_mpd, "outputs", buf, sizeof(buf)) != 0) return 0;
    int cur = -1, en = 0;
    char *line = strtok(buf, "\n");
    while (line) {
        int v;
        if (sscanf(line, "outputid: %d", &v) == 1) {
            if (cur == id && en) return 1;
            cur = v; en = 0;
        } else if (sscanf(line, "outputenabled: %d", &v) == 1) {
            en = v;
        }
        line = strtok(NULL, "\n");
    }
    return (cur == id && en) ? 1 : 0;
}


static int pulse_default_is_bt(void)
{
    FILE *f = popen("pactl get-default-sink 2>/dev/null", "r");
    char b[256] = {0};
    if (!f) return 0;
    if (!fgets(b, sizeof(b), f)) b[0] = 0;
    pclose(f);
    return strstr(b, "bluez") != NULL;
}

/* Jedno mesto za prikaz DAC/BT stanja (ikona + boja) - bez treperenja */
static void apply_out_ui(int bt)
{
    if (lbl_out_icon) {
        lv_label_set_text(lbl_out_icon, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(lbl_out_icon, lv_color_hex(0x42A5F5), 0);
        if (bt) lv_obj_clear_flag(lbl_out_icon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lbl_out_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (img_out_icon) {
        if (bt) lv_obj_add_flag(img_out_icon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(img_out_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (img_menu_icon) {
        if (bt) lv_obj_add_flag(img_menu_icon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(img_menu_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_out) {
        lv_obj_set_style_bg_color(btn_out, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_border_color(btn_out,
            lv_color_hex(bt ? 0x1E88E5 : 0x43A047), 0);
        lv_obj_set_style_border_width(btn_out, 2, 0);
    }
    if (btn_out_icon) {
        lv_obj_set_style_border_color(btn_out_icon,
            lv_color_hex(bt ? 0x1E88E5 : 0x43A047), 0);
    }
    if (btn_out_lbl) {
        /* samo simbol: BT znak kad je BT, sakriven kad je DAC (tada je jack ikonica) */
        if (bt) lv_obj_clear_flag(btn_out_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btn_out_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void out_event(lv_event_t *e)
{
    (void)e;
    int bt = pulse_default_is_bt();
    system(bt ? "/opt/radio/pulse-switch.sh dac" : "/opt/radio/pulse-switch.sh bt");
    apply_out_ui(!bt);
}

static void vol_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (c == LV_EVENT_PRESSED) vol_drag = 1;
    if (c == LV_EVENT_RELEASED) vol_drag = 0;
    if (c == LV_EVENT_VALUE_CHANGED && g_connected) {
        char b[32]; snprintf(b, sizeof(b), "%d%%", v);
        char pb[32]; snprintf(pb, sizeof(pb), "Vol: %d%%", v);
        if (lbl_vol) lv_label_set_text(lbl_vol, pb);
        if (lbl_vol_menu) lv_label_set_text(lbl_vol_menu, b);
        mpd_setvol(g_mpd, v);
    }
}


static int station_index_for_url(const char *url)
{
    if (!url) return -1;
    for (int i = 0; i < STATION_COUNT; i++) {
        size_t n = strlen(stations[i].url);
        if (n && !strncmp(url, stations[i].url, n)) return i;
    }
    return -1;
}

static void stations_refresh(void)
{
    /* zuto (oker): stanica koja svira, ili poslednja koriscena ako svira lokal */
    int hl = (active_station >= 0) ? active_station : last_station;
    for (int i = 0; i < STATION_COUNT; i++) {
        lv_obj_t *b = station_btns[i];
        if (!b) continue;
        int on = (i == hl);
        lv_obj_set_style_bg_color(b, lv_color_hex(on ? 0x3B3117 : COL_PANEL), 0);
        lv_obj_set_style_border_width(b, on ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(on ? 0xD4A72C : COL_BORDER), 0);
        lv_obj_set_style_text_color(b, lv_color_hex(on ? 0xF2CB61 : COL_TEXT), 0);
        lv_obj_t *bl = lv_obj_get_child(b, 0);
        if (bl) lv_obj_set_style_text_color(bl, lv_color_hex(on ? 0xF2CB61 : COL_TEXT), 0);
    }
}

static void stations_center_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    stations_center_active();
}

static void stations_center_active(void)
{
    int hl = (active_station >= 0) ? active_station : last_station;
    if (!stations_list || hl < 0 || hl >= STATION_COUNT) return;
    lv_obj_t *b = station_btns[hl];
    if (!b) return;
    lv_obj_update_layout(stations_list);
    int view_h = lv_obj_get_height(stations_list);
    int item_h = lv_obj_get_height(b);
    int cur_scroll = lv_obj_get_scroll_y(stations_list);
    int pad_top = lv_obj_get_style_pad_top(stations_list, 0);
    int row_h = item_h + lv_obj_get_style_pad_row(stations_list, 0);
    int content_y = pad_top + hl * row_h;
    int y = content_y - (view_h - item_h) / 2;
    if (y < 0) y = 0;
    lv_obj_scroll_to_y(stations_list, y, LV_ANIM_OFF);
}

static void station_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= STATION_COUNT) return;
    if (g_connected) mpd_play_url(g_mpd, stations[idx].url);
    active_station = idx;
    last_station = idx;
    last_station_save(idx);
    stations_refresh();
    set_cover_station(stations[idx].name);
    nav_home();
}

/* dodaj u MPD queue: selektovane numere ili ceo folder ako nista nije selektovano */
static void local_add_to_queue(void)
{
    if (!g_connected) return;
    int any_sel = 0;
    for (int i = 0; i < local_count; i++)
        if (!local_isdir[i] && local_sel[i]) any_sel = 1;

    local_mpd_update_dir();

    mpd_status_t st;
    int qlen = 0, playing = 0;
    if (mpd_get_status(g_mpd, &st) == 0) { qlen = st.queue_len; playing = (st.playing == 1); }

    int is_stream = (strncmp(cur_url, "http", 4) == 0);
    if (is_stream) { mpd_cmd(g_mpd, "clear"); qlen = 0; playing = 0; cur_url[0] = 0; }

    char rel[600], cmd[700];
    int base = qlen, first = -1, added = 0;
    for (int i = 0; i < local_count; i++) {
        if (local_isdir[i]) continue;
        if (any_sel && !local_sel[i]) continue;
        local_rel_path(i, rel, sizeof(rel));
        snprintf(cmd, sizeof(cmd), "add \"%s\"", rel);
        int rc = mpd_cmd(g_mpd, cmd);
        for (int t = 0; t < 6 && rc != 0; t++) { usleep(700000); rc = mpd_cmd(g_mpd, cmd); }
        if (rc != 0) continue;
        if (first < 0) first = base + added;
        added++;
    }
    if (!added) return;

    mpd_cmd(g_mpd, "single 0");
    mpd_cmd(g_mpd, "repeat 0");
    if (first >= 0 && (is_stream || !playing)) {
        snprintf(cmd, sizeof(cmd), "play %d", first);
        mpd_cmd(g_mpd, cmd);
    }
    /* ocisti selekciju i skini oker (ekran ostaje gde jeste) */
    for (int k = 0; k < LOCAL_MAX; k++) local_sel[k] = 0;
    local_fill();
}

static void local_addq_event(lv_event_t *e) { (void)e; local_add_to_queue(); }

static void build_local(void)
{
    scr_local = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_local, lv_color_hex(COL_BG), 0);
    apply_bg(scr_local);
    lv_obj_set_style_pad_all(scr_local, 0, 0);
    lv_obj_clear_flag(scr_local, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(scr_local);
    lv_obj_set_pos(hdr, 24, 16);
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_28, 0);
    lv_label_set_text(hdr, "Local");

    local_path_lbl = lv_label_create(scr_local);
    lv_obj_set_pos(local_path_lbl, 24, 48);
    lv_obj_set_width(local_path_lbl, 360);
    lv_label_set_long_mode(local_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(local_path_lbl, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(local_path_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(local_path_lbl, "NAS /");

    mk_btn(scr_local, 400, 16, 156, 44, "Add to Queue", local_addq_event, NULL, 0);
    mk_btn(scr_local, 566, 16, 96, 44, "Up", local_up_event, NULL, 0);
    mk_btn(scr_local, 668, 16, 108, 44, "Back", back_event, NULL, 0);

    local_list = lv_obj_create(scr_local);
    lv_obj_set_pos(local_list, 16, 88);
    lv_obj_set_size(local_list, 768, 378);
    lv_obj_set_style_bg_color(local_list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(local_list, 0, 0);
    lv_obj_set_style_pad_all(local_list, 0, 0);
    lv_obj_set_style_pad_row(local_list, 8, 0);
    lv_obj_set_flex_flow(local_list, LV_FLEX_FLOW_COLUMN);

    const char *seed = getenv("OIR_LOCAL_DIR");
    if (seed && *seed == '/') snprintf(local_path, sizeof(local_path), "%s", seed);
    else snprintf(local_path, sizeof(local_path), "%s", LOCAL_ROOT);
    local_scan();
    local_fill();
    /* lista se puni tek kad se ekran otvori (stedi LVGL heap pri bootu) */
}

static void build_splash(void)
{
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr_splash, 0, 0);
    lv_obj_clear_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);
    if (!splash_ready) return;
    lv_obj_t *img = lv_image_create(scr_splash);
    lv_image_set_src(img, &splash_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------------- PLAYLISTS (myMPD / MPD) ---------------- */
static void pl_refresh_styles(void)
{
    for (int i = 0; i < pl_count; i++) {
        lv_obj_t *b = pl_btns[i];
        if (!b) continue;
        int on = pl_sel[i];
        style_btn(b, on ? 0x3B3117 : COL_PANEL, on ? 0xF2CB61 : COL_TEXT, on ? 1 : 0);
        if (on) lv_obj_set_style_border_color(b, lv_color_hex(0xD4A72C), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? 0xF2CB61 : COL_TEXT), 0);
    }
}

static void pl_item_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= pl_count) return;
    pl_sel[idx] = !pl_sel[idx];
    pl_refresh_styles();
}

static void pl_load_event(lv_event_t *e)
{
    (void)e;
    if (!g_connected) return;
    int any = 0;
    for (int i = 0; i < pl_count; i++) if (pl_sel[i]) any = 1;
    if (!any) return;

    int is_stream = (strncmp(cur_url, "http", 4) == 0);
    if (is_stream) { mpd_cmd(g_mpd, "clear"); cur_url[0] = 0; }

    char cmd[300];
    for (int i = 0; i < pl_count; i++) {
        if (!pl_sel[i]) continue;
        snprintf(cmd, sizeof(cmd), "load \"%s\"", pl_names[i]);
        mpd_cmd(g_mpd, cmd);
        pl_sel[i] = 0;
    }
    queue_fill();
    nav_back();          /* nazad na Queue */
}

static void pl_fill(void)
{
    if (!pl_list) return;
    lv_obj_clean(pl_list);
    pl_count = 0;
    for (int i = 0; i < PL_MAX; i++) { pl_btns[i] = NULL; pl_sel[i] = 0; }
    if (!g_connected) return;

    static char buf[MPD_BUF_Q];
    if (mpd_cmd_read(g_mpd, "listplaylists", buf, sizeof(buf)) != 0) return;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "playlist: ", 10)) continue;
        if (pl_count >= PL_MAX) break;
        snprintf(pl_names[pl_count], sizeof(pl_names[0]), "%s", line + 10);
        pl_count++;
    }

    for (int i = 0; i < pl_count; i++) {
        lv_obj_t *b = lv_button_create(pl_list);
        lv_obj_set_size(b, 760, 56);
        style_btn(b, COL_PANEL, COL_TEXT, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, pl_names[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_add_event_cb(b, pl_item_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        pl_btns[i] = b;
    }
    if (pl_count == 0) {
        lv_obj_t *l = lv_label_create(pl_list);
        lv_label_set_text(l, "No saved playlists");
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    }
}

static void pl_event(lv_event_t *e)
{
    (void)e;
    pl_fill();
    nav_to(scr_pl);
}

static void build_playlists(void)
{
    scr_pl = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_pl, lv_color_hex(COL_BG), 0);
    apply_bg(scr_pl);
    lv_obj_set_style_pad_all(scr_pl, 0, 0);
    lv_obj_clear_flag(scr_pl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_label_create(scr_pl);
    lv_obj_set_pos(h, 24, 16);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_label_set_text(h, "Playlists");

    mk_btn(scr_pl, 540, 16, 116, 44, "Load2Q", pl_load_event, NULL, 1);
    mk_btn(scr_pl, 668, 16, 108, 44, "Back", back_event, NULL, 0);

    pl_list = lv_obj_create(scr_pl);
    lv_obj_set_pos(pl_list, 16, 84);
    lv_obj_set_size(pl_list, 768, 382);
    lv_obj_set_style_bg_color(pl_list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(pl_list, 0, 0);
    lv_obj_set_style_pad_all(pl_list, 0, 0);
    lv_obj_set_style_pad_row(pl_list, 8, 0);
    lv_obj_set_flex_flow(pl_list, LV_FLEX_FLOW_COLUMN);
}

/* ---------------- QUEUE ---------------- */
static void queue_play_event(lv_event_t *e)
{
    int pos = (int)(intptr_t)lv_event_get_user_data(e);
    if (!g_connected || pos < 0) return;
    mpd_play_pos(g_mpd, pos);
    snprintf(cur_logo, sizeof(cur_logo), "%s", "");
}

static void queue_clear_event(lv_event_t *e)
{
    (void)e;
    if (!g_connected) return;
    mpd_cmd(g_mpd, "clear");
    queue_fill();
}

static void queue_row(int pos, const char *title, const char *artist, int cur)
{
    lv_obj_t *b = lv_button_create(queue_list);
    lv_obj_set_size(b, 760, 52);
    style_btn(b, cur ? 0x1F3A38 : COL_PANEL, cur ? COL_ACCENT : COL_TEXT, cur ? 1 : 0);
    lv_obj_t *l = lv_label_create(b);
    char txt[420];
    if (artist[0] && title[0]) snprintf(txt, sizeof(txt), "%2d.  %s - %s", pos + 1, artist, title);
    else snprintf(txt, sizeof(txt), "%2d.  %s", pos + 1, title[0] ? title : artist);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_event_cb(b, queue_play_event, LV_EVENT_CLICKED, (void *)(intptr_t)pos);
}

static void queue_fill(void)
{
    if (!queue_list) return;
    lv_obj_clean(queue_list);
    if (!g_connected) return;

    static char buf[MPD_BUF_Q];
    if (mpd_queue(g_mpd, buf, sizeof(buf)) != 0) return;

    mpd_status_t st;
    int cur = -1;
    if (mpd_get_status(g_mpd, &st) == 0) cur = st.song;

    char file[512] = "", title[256] = "", artist[256] = "";
    int pos = -1;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "file: ", 6)) {
            if (pos >= 0) {
                if (!title[0]) {
                    const char *sl = strrchr(file, '/');
                    snprintf(title, sizeof(title), "%s", sl ? sl + 1 : file);
                }
                queue_row(pos, title, artist, pos == cur);
            }
            snprintf(file, sizeof(file), "%s", line + 6);
            title[0] = artist[0] = 0;
            pos++;
        } else if (!strncmp(line, "Title: ", 7)) {
            snprintf(title, sizeof(title), "%s", line + 7);
        } else if (!strncmp(line, "Artist: ", 8)) {
            snprintf(artist, sizeof(artist), "%s", line + 8);
        }
    }
    if (pos >= 0) {
        if (!title[0]) {
            const char *sl = strrchr(file, '/');
            snprintf(title, sizeof(title), "%s", sl ? sl + 1 : file);
        }
        queue_row(pos, title, artist, pos == cur);
    }
    if (pos < 0) {
        lv_obj_t *l = lv_label_create(queue_list);
        lv_label_set_text(l, "Queue is empty");
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    }
}

static void queue_event(lv_event_t *e)
{
    (void)e;
    queue_fill();
    nav_to(scr_queue);
}

static void build_queue(void)
{
    scr_queue = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_queue, lv_color_hex(COL_BG), 0);
    apply_bg(scr_queue);
    lv_obj_set_style_pad_all(scr_queue, 0, 0);
    lv_obj_clear_flag(scr_queue, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_label_create(scr_queue);
    lv_obj_set_pos(h, 24, 16);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_label_set_text(h, "Queue");

    mk_btn(scr_queue, 434, 16, 106, 44, "Load", pl_event, NULL, 0);
    mk_btn(scr_queue, 550, 16, 106, 44, "Clear", queue_clear_event, NULL, 0);
    mk_btn(scr_queue, 668, 16, 108, 44, "Back", back_event, NULL, 0);

    queue_list = lv_obj_create(scr_queue);
    lv_obj_set_pos(queue_list, 16, 84);
    lv_obj_set_size(queue_list, 768, 382);
    lv_obj_set_style_bg_color(queue_list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(queue_list, 0, 0);
    lv_obj_set_style_pad_all(queue_list, 0, 0);
    lv_obj_set_style_pad_row(queue_list, 8, 0);
    lv_obj_set_flex_flow(queue_list, LV_FLEX_FLOW_COLUMN);
}

static void build_player(void)
{
    scr_player = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_player, lv_color_hex(COL_BG), 0);
    apply_bg(scr_player);
    lv_obj_set_style_pad_all(scr_player, 0, 0);
    lv_obj_clear_flag(scr_player, LV_OBJ_FLAG_SCROLLABLE);

    /* cover levo */
    canvas_cover = lv_canvas_create(scr_player);
    lv_canvas_set_buffer(canvas_cover, cover_buf, COVER_W, COVER_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas_cover, COVER_X, COVER_Y);
    lv_obj_set_style_radius(canvas_cover, 12, 0);
    lv_obj_set_style_clip_corner(canvas_cover, true, 0);
    cover_clear();

    /* --- gore levo: izlaz (DAC/BT) + sat --- */
    lv_obj_t *bo = lv_button_create(scr_player);
    lv_obj_set_pos(bo, 688, 8);
    lv_obj_set_size(bo, 40, 40);
    lv_obj_set_style_radius(bo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bo, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(bo, 255, 0);
    lv_obj_set_style_border_width(bo, 2, 0);
    lv_obj_set_style_border_color(bo, lv_color_hex(0x43A047), 0);
    lv_obj_add_event_cb(bo, out_event, LV_EVENT_CLICKED, NULL);
    btn_out_icon = bo;
    lbl_out_icon = lv_label_create(bo);
    lv_obj_set_style_text_font(lbl_out_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_out_icon, lv_color_hex(0x42A5F5), 0);
    lv_label_set_text(lbl_out_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_center(lbl_out_icon);
    lv_obj_add_flag(lbl_out_icon, LV_OBJ_FLAG_HIDDEN);

    img_out_icon = lv_image_create(bo);
    lv_image_set_src(img_out_icon, &icon_jack_24);
    lv_obj_set_style_image_recolor(img_out_icon, lv_color_hex(0x66BB6A), 0);
    lv_obj_set_style_image_recolor_opa(img_out_icon, LV_OPA_COVER, 0);
    lv_obj_center(img_out_icon);

    /* volume indikator (samo prikaz; rotary dolazi kasnije) */
    lbl_vol = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_vol, 500, 16);
    lv_obj_set_width(lbl_vol, 176);
    lv_obj_set_style_text_align(lbl_vol, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_vol, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_vol, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_vol, "Vol: --%");

    lbl_clock = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_clock, 736, 16);
    lv_obj_set_width(lbl_clock, 56);
    lv_obj_set_style_text_align(lbl_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_clock, "--:--");

    /* tri linije (stanica / izvodjac / naslov) vertikalno centrirane uz cover */
    lv_obj_t *meta_box = lv_obj_create(scr_player);
    lv_obj_set_pos(meta_box, 368, 24);
    lv_obj_set_size(meta_box, 408, 192);
    lv_obj_set_style_bg_opa(meta_box, 0, 0);
    lv_obj_set_style_border_width(meta_box, 0, 0);
    lv_obj_set_style_pad_all(meta_box, 0, 0);
    lv_obj_set_style_pad_row(meta_box, 6, 0);
    lv_obj_clear_flag(meta_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(meta_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(meta_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    lbl_top = lv_label_create(meta_box);
    lv_obj_set_width(lbl_top, 408);
    lv_label_set_long_mode(lbl_top, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_top, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_top, &lv_font_montserrat_26, 0);
    lv_label_set_text(lbl_top, "OIR");

    /* --- naslov / izvodjac / album / format --- */
    lbl_sub = lv_label_create(meta_box);
    lv_obj_set_width(lbl_sub, 408);
    lv_label_set_long_mode(lbl_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_sub, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_26, 0);
    lv_label_set_text(lbl_sub, "");

    lbl_title = lv_label_create(meta_box);
    lv_obj_set_width(lbl_title, 408);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_36, 0);
    lv_label_set_text(lbl_title, "Ostoja Internet Radio");

    lbl_album = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_album, 368, 186);
    lv_obj_set_width(lbl_album, 408);
    lv_label_set_long_mode(lbl_album, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_album, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_album, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_album, "");

    lbl_src = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_src, 368, 288);
    lv_obj_set_width(lbl_src, 408);
    lv_label_set_long_mode(lbl_src, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_src, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_src, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_src, "RADIO STREAM");

    lbl_data = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_data, 368, 318);
    lv_obj_set_width(lbl_data, 408);
    lv_label_set_long_mode(lbl_data, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_data, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_data, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_data, "");

    /* --- seek --- */
    bar_prog = lv_bar_create(scr_player);
    lv_obj_set_pos(bar_prog, 368, 246);
    lv_obj_set_size(bar_prog, 408, 6);
    lv_bar_set_range(bar_prog, 0, 1000);
    lv_obj_set_style_bg_color(bar_prog, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_color(bar_prog, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_prog, 3, 0);

    lbl_elapsed = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_elapsed, 368, 256);
    lv_obj_set_style_text_color(lbl_elapsed, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_elapsed, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_elapsed, "");

    lbl_total = lv_label_create(scr_player);
    lv_obj_set_pos(lbl_total, 690, 256);
    lv_obj_set_width(lbl_total, 86);
    lv_obj_set_style_text_align(lbl_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_total, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(lbl_total, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_total, "");

    /* --- transport --- */
    mk_round_icon_btn(scr_player, 274, 398, 64, &icon_prev, COL_TEXT, prev_event, NULL, 0);
    lv_obj_t *bp = mk_round_icon_btn(scr_player, 358, 388, 84, &icon_play, 0x101010, play_event, NULL, 1);
    img_play = lv_obj_get_child(bp, 0);
    mk_round_icon_btn(scr_player, 462, 398, 64, &icon_next, COL_TEXT, next_event, NULL, 0);

    /* --- okruglo MENU dugme (dole desno) --- */
    lv_obj_t *bm = lv_button_create(scr_player);
    lv_obj_set_pos(bm, 708, 396);
    lv_obj_set_size(bm, 68, 68);
    lv_obj_set_style_radius(bm, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bm, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(bm, 255, 0);
    lv_obj_set_style_border_width(bm, 2, 0);
    lv_obj_set_style_border_color(bm, lv_color_hex(COL_BORDER), 0);
    lv_obj_add_event_cb(bm, menu_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(bm);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ml, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ml, LV_SYMBOL_LIST);
    lv_obj_center(ml);
}

static void build_menu(void)
{
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, lv_color_hex(COL_BG), 0);
    apply_bg(scr_menu);
    lv_obj_set_style_pad_all(scr_menu, 0, 0);
    lv_obj_clear_flag(scr_menu, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(scr_menu);
    lv_obj_set_pos(t, 24, 21);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, "Settings");

    /* okruglo Power dugme, horizontalno centrirano u redu sa Settings/Back */
    lv_obj_t *bp = lv_button_create(scr_menu);
    lv_obj_set_pos(bp, 378, 16);
    lv_obj_set_size(bp, 44, 44);
    lv_obj_set_style_radius(bp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bp, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(bp, 255, 0);
    lv_obj_set_style_border_width(bp, 2, 0);
    lv_obj_set_style_border_color(bp, lv_color_hex(0xC62828), 0);
    lv_obj_add_event_cb(bp, power_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pi = lv_label_create(bp);
    lv_obj_set_style_text_font(pi, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pi, lv_color_hex(0xEF5350), 0);
    lv_label_set_text(pi, LV_SYMBOL_POWER);
    lv_obj_center(pi);

    mk_btn(scr_menu, 654, 16, 122, 44, "Back", back_event, NULL, 0);

    lv_obj_t *vl = lv_label_create(scr_menu);
    lv_obj_set_pos(vl, 24, 376);
    lv_obj_set_style_text_color(vl, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(vl, &lv_font_montserrat_16, 0);
    lv_label_set_text(vl, "VOLUME");

    lbl_vol_menu = lv_label_create(scr_menu);
    lv_obj_set_pos(lbl_vol_menu, 686, 376);
    lv_obj_set_width(lbl_vol_menu, 90);
    lv_obj_set_style_text_align(lbl_vol_menu, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_vol_menu, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(lbl_vol_menu, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_vol_menu, "--%");

    slider_vol_menu = lv_slider_create(scr_menu);
    lv_obj_set_pos(slider_vol_menu, 24, 400);
    lv_obj_set_size(slider_vol_menu, 752, 34);
    lv_slider_set_range(slider_vol_menu, 0, 100);
    lv_obj_set_style_bg_color(slider_vol_menu, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(slider_vol_menu, 255, 0);
    lv_obj_set_style_radius(slider_vol_menu, 8, 0);
    lv_obj_set_style_bg_color(slider_vol_menu, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_vol_menu, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_vol_menu, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_vol_menu, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_vol_menu, vol_event, LV_EVENT_ALL, NULL);

    mk_btn(scr_menu, 24, 84, 368, 88, LV_SYMBOL_LIST "    Stations", stations_event, NULL, 0);
    mk_btn(scr_menu, 24, 180, 368, 88, LV_SYMBOL_DIRECTORY "    Local NAS", local_event, NULL, 0);
    lv_obj_t *bo = mk_btn(scr_menu, 408, 84, 368, 88, "", out_event, NULL, 0);
    btn_out = bo;

    /* jack ikonica (DAC) */
    img_menu_icon = lv_image_create(bo);
    lv_image_set_src(img_menu_icon, &icon_jack_48);
    lv_obj_set_style_image_recolor(img_menu_icon, lv_color_hex(0x66BB6A), 0);
    lv_obj_set_style_image_recolor_opa(img_menu_icon, LV_OPA_COVER, 0);
    lv_obj_align(img_menu_icon, LV_ALIGN_LEFT_MID, 40, 0);

    /* bluetooth simbol (vidljiv samo kad je BT izlaz) */
    btn_out_lbl = lv_label_create(bo);
    lv_obj_set_style_text_font(btn_out_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(btn_out_lbl, lv_color_hex(0x42A5F5), 0);
    lv_label_set_text(btn_out_lbl, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(btn_out_lbl, LV_ALIGN_LEFT_MID, 40, 0);
    lv_obj_add_flag(btn_out_lbl, LV_OBJ_FLAG_HIDDEN);

    /* stalni tekst */
    lv_obj_t *ol = lv_label_create(bo);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ol, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ol, "Output select");
    lv_obj_align(ol, LV_ALIGN_LEFT_MID, 120, 0);
    mk_btn(scr_menu, 408, 180, 368, 88, LV_SYMBOL_SETTINGS "    Settings", settings_event, NULL, 0);
    mk_btn(scr_menu, 24, 276, 368, 88, LV_SYMBOL_LIST "    Queue", queue_event, NULL, 0);
}

static void build_stations(void)
{
    scr_stations = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_stations, lv_color_hex(COL_BG), 0);
    apply_bg(scr_stations);
    lv_obj_set_style_pad_all(scr_stations, 0, 0);
    lv_obj_clear_flag(scr_stations, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(scr_stations);
    lv_obj_set_pos(hdr, 24, 16);
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_28, 0);
    lv_label_set_text(hdr, "Stations");

    mk_btn(scr_stations, 654, 16, 122, 44, "Back", back_event, NULL, 0);

    /* ista geometrija kao LOKAL: bez panela/okvira, lista pocinje od vrha */
    lv_obj_t *listw = lv_obj_create(scr_stations);
    lv_obj_set_pos(listw, 16, 88);
    lv_obj_set_size(listw, 768, 378);
    lv_obj_set_style_bg_opa(listw, 0, 0);
    lv_obj_set_style_border_width(listw, 0, 0);
    lv_obj_set_style_pad_all(listw, 0, 0);
    lv_obj_set_style_pad_row(listw, 8, 0);
    /* bez praznog prostora iznad prve stanice; bottom padding da i poslednja
       moze da se centrira kad je highlightovana */
    lv_obj_set_style_pad_top(listw, 0, 0);
    lv_obj_set_style_pad_bottom(listw, 0, 0);
    lv_obj_set_flex_flow(listw, LV_FLEX_FLOW_COLUMN);

    stations_list = listw;
    for (int i = 0; i < STATION_COUNT; i++) {
        lv_obj_t *b = mk_btn(listw, 0, 0, 760, 56, stations[i].name, station_click, (void *)(intptr_t)i, 0);
        lv_obj_set_width(b, 760);
        lv_obj_set_height(b, 56);
        /* isto kao LOKAL: tekst levo, 16 px inset */
        lv_obj_t *bl = lv_obj_get_child(b, 0);
        if (bl) lv_obj_align(bl, LV_ALIGN_LEFT_MID, 16, 0);
        station_btns[i] = b;
    }
    stations_refresh();
}


/* ---------- WIFI (nmcli) ---------- */
#define WIFI_MAX 40

static lv_obj_t *scr_wifi, *scr_wifi_pass;
static lv_obj_t *wifi_list, *wifi_status_lbl, *wifi_pass_lbl, *wifi_ta, *wifi_kb;
static char wifi_pick[128];
static char wifi_ssids[WIFI_MAX][64];
static int wifi_signals[WIFI_MAX];
static int wifi_secure[WIFI_MAX];
static int wifi_count;

static void wifi_pick_event(lv_event_t *e);

static void wifi_esc(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < n; i++) {
        char c = in[i];
        if (c == '"' || c == '\\' || c == '$' || c == 96) out[j++] = '\\';
        out[j++] = c;
    }
    out[j] = 0;
}

static void wifi_current(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = popen("nmcli -t -f GENERAL.CONNECTION dev show wlan0 2>/dev/null", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        char *colon = strchr(line, ':');
        const char *name = colon ? colon + 1 : line;
        /* netplan-wlan0-SSID -> SSID */
        const char *dash = strstr(name, "wlan0-");
        if (dash) name = dash + 6;
        if (strcmp(name, "--") != 0 && name[0]) snprintf(out, n, "%s", name);
    }
    pclose(f);
}

static void wifi_scan_now(void)
{
    wifi_count = 0;
    FILE *f = popen("nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list --rescan yes 2>/dev/null", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL && wifi_count < WIFI_MAX) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        int i_last = -1, i_prev = -1;
        for (int p = (int)L - 1; p >= 0; p--) {
            if (line[p] == ':' && (p == 0 || line[p-1] != '\\')) {
                if (i_last < 0) i_last = p;
                else { i_prev = p; break; }
            }
        }
        if (i_prev < 0 || i_last < 0) continue;
        int sig = atoi(line + i_prev + 1);
        char sec[64];
        snprintf(sec, sizeof(sec), "%s", line + i_last + 1);
        char ssid[64];
        size_t sl = (size_t)i_prev;
        if (sl >= sizeof(ssid)) sl = sizeof(ssid) - 1;
        size_t j = 0;
        for (size_t i = 0; i < sl && j + 1 < sizeof(ssid); i++) {
            if (line[i] == '\\' && i + 1 < sl) i++;
            ssid[j++] = line[i];
        }
        ssid[j] = 0;
        if (!ssid[0] || !strcmp(ssid, "--")) continue;
        int dup = 0;
        for (int k = 0; k < wifi_count; k++) if (!strcmp(wifi_ssids[k], ssid)) { dup = 1; break; }
        if (dup) continue;
        snprintf(wifi_ssids[wifi_count], sizeof(wifi_ssids[0]), "%s", ssid);
        wifi_signals[wifi_count] = sig;
        wifi_secure[wifi_count] = (sec[0] && strncmp(sec, "--", 2) != 0) ? 1 : 0;
        wifi_count++;
    }
    pclose(f);
}

static void wifi_fill(void)
{
    if (!wifi_list) return;
    lv_obj_clean(wifi_list);
    char cur[128];
    wifi_current(cur, sizeof(cur));
    if (wifi_status_lbl) {
        char b[220];
        if (cur[0]) snprintf(b, sizeof(b), "Connected: %s", cur);
        else snprintf(b, sizeof(b), "Nije povezano (wlan0)");
        lv_label_set_text(wifi_status_lbl, b);
    }
    if (wifi_count == 0) {
        lv_obj_t *l = lv_label_create(wifi_list);
        lv_label_set_text(l, "Nema mreza u dometu");
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        return;
    }
    for (int i = 0; i < wifi_count; i++) {
        lv_obj_t *b = lv_button_create(wifi_list);
        lv_obj_set_size(b, 760, 52);
        style_btn(b, COL_PANEL, COL_TEXT, 0);
        lv_obj_t *l = lv_label_create(b);
        char t[180];
        snprintf(t, sizeof(t), "%s%s    %d%%", wifi_ssids[i], wifi_secure[i] ? "  *" : "", wifi_signals[i]);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_add_event_cb(b, wifi_pick_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void wifi_connect_do(const char *pass)
{
    char e_ssid[300], e_pass[300], cmd[700];
    wifi_esc(wifi_pick, e_ssid, sizeof(e_ssid));
    wifi_esc(pass, e_pass, sizeof(e_pass));
    if (pass[0]) snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect \"%s\" password \"%s\" >/tmp/wifi.log 2>&1", e_ssid, e_pass);
    else snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect \"%s\" >/tmp/wifi.log 2>&1", e_ssid);
    system(cmd);
    wifi_scan_now();
    wifi_fill();
    nav_back();
}

static void wifi_save_event(lv_event_t *e)
{
    (void)e;
    const char *p = wifi_ta ? lv_textarea_get_text(wifi_ta) : "";
    wifi_connect_do(p ? p : "");
}

static void wifi_back_event(lv_event_t *e) { (void)e; nav_back(); }

static void wifi_pick_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= wifi_count) return;
    snprintf(wifi_pick, sizeof(wifi_pick), "%s", wifi_ssids[idx]);
    if (wifi_pass_lbl) {
        char b[220];
        snprintf(b, sizeof(b), "Password for: %s", wifi_pick);
        lv_label_set_text(wifi_pass_lbl, b);
    }
    if (wifi_ta) {
        lv_textarea_set_text(wifi_ta, "");
        lv_textarea_set_password_mode(wifi_ta, wifi_secure[idx] ? true : false);
    }
    nav_to(scr_wifi_pass);
}

static void wifi_rescan_event(lv_event_t *e)
{
    (void)e;
    wifi_scan_now();
    wifi_fill();
}

static void wifi_event(lv_event_t *e)
{
    (void)e;
    wifi_scan_now();
    wifi_fill();
    nav_to(scr_wifi);
}

static void build_wifi(void)
{
    scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi, lv_color_hex(COL_BG), 0);
    apply_bg(scr_wifi);
    lv_obj_set_style_pad_all(scr_wifi, 0, 0);
    lv_obj_clear_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_label_create(scr_wifi);
    lv_obj_set_pos(h, 24, 16);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_label_set_text(h, "WIFI");

    wifi_status_lbl = lv_label_create(scr_wifi);
    lv_obj_set_pos(wifi_status_lbl, 24, 50);
    lv_obj_set_width(wifi_status_lbl, 500);
    lv_label_set_long_mode(wifi_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(wifi_status_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(wifi_status_lbl, "");

    mk_btn(scr_wifi, 450, 16, 110, 44, "Scan", wifi_rescan_event, NULL, 0);
    mk_btn(scr_wifi, 572, 16, 90, 44, "Back", back_event, NULL, 0);

    wifi_list = lv_obj_create(scr_wifi);
    lv_obj_set_pos(wifi_list, 16, 84);
    lv_obj_set_size(wifi_list, 768, 382);
    lv_obj_set_style_bg_color(wifi_list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(wifi_list, 0, 0);
    lv_obj_set_style_pad_all(wifi_list, 0, 0);
    lv_obj_set_style_pad_row(wifi_list, 8, 0);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);

    scr_wifi_pass = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi_pass, lv_color_hex(COL_BG), 0);
    apply_bg(scr_wifi_pass);
    lv_obj_set_style_pad_all(scr_wifi_pass, 0, 0);
    lv_obj_clear_flag(scr_wifi_pass, LV_OBJ_FLAG_SCROLLABLE);

    wifi_pass_lbl = lv_label_create(scr_wifi_pass);
    lv_obj_set_pos(wifi_pass_lbl, 24, 14);
    lv_obj_set_width(wifi_pass_lbl, 520);
    lv_label_set_long_mode(wifi_pass_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(wifi_pass_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(wifi_pass_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(wifi_pass_lbl, "Lozinka");

    mk_btn(scr_wifi_pass, 578, 14, 96, 44, "Back", wifi_back_event, NULL, 0);

    wifi_ta = lv_textarea_create(scr_wifi_pass);
    lv_obj_set_pos(wifi_ta, 24, 64);
    lv_obj_set_size(wifi_ta, 620, 52);
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_password_mode(wifi_ta, true);
    lv_textarea_set_placeholder_text(wifi_ta, "lozinka");
    lv_obj_set_style_text_font(wifi_ta, &lv_font_montserrat_20, 0);

    mk_btn(scr_wifi_pass, 660, 64, 120, 52, "OK", wifi_save_event, NULL, 1);

    wifi_kb = lv_keyboard_create(scr_wifi_pass);
    lv_keyboard_set_textarea(wifi_kb, wifi_ta);
    lv_keyboard_set_mode(wifi_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    /* keyboard se sam poravna na dno - vratimo ga na nase koordinate */
    lv_obj_set_align(wifi_kb, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(wifi_kb, 6, 118);
    lv_obj_set_size(wifi_kb, 788, 356);
    lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(wifi_kb, 0, 0);
    lv_obj_set_style_pad_all(wifi_kb, 4, 0);
    lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(COL_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(wifi_kb, 255, LV_PART_ITEMS);
    lv_obj_set_style_text_color(wifi_kb, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(wifi_kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_border_width(wifi_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(wifi_kb, lv_color_hex(COL_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(COL_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(wifi_kb, lv_color_hex(0x101010), LV_PART_ITEMS | LV_STATE_PRESSED);
}


/* ---------- BLUETOOTH ---------- */
#define BT_MAX 48
#define BT_CACHE "/opt/radio/bt-known.tsv"

static lv_obj_t *scr_bt, *bt_list, *bt_status_lbl;
static lv_obj_t *btn_bt_dis;
static char bt_sel_mac[20];
static int bt_sel = -1;
static lv_timer_t *bt_pair_timer;
static char bt_macs[BT_MAX][20];
static char bt_names[BT_MAX][96];
static int bt_connected[BT_MAX];
static int bt_known[BT_MAX];
static int bt_seen[BT_MAX];
static int bt_count;

static void bt_pick_event(lv_event_t *e);

static int bt_name_is_mac(const char *s)
{
    if (!s || strlen(s) < 17) return 0;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':' && s[i] != '-') return 0; }
        else {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
        }
    }
    return 1;
}

static void bt_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int bt_find(const char *mac)
{
    for (int i = 0; i < bt_count; i++)
        if (!strcmp(bt_macs[i], mac)) return i;
    return -1;
}

static void bt_add(const char *mac, const char *name, int connected, int known)
{
    if (!mac || strlen(mac) < 17) return;
    int i = bt_find(mac);
    if (i < 0) {
        if (bt_count >= BT_MAX) return;
        i = bt_count++;
        snprintf(bt_macs[i], sizeof(bt_macs[0]), "%s", mac);
        bt_names[i][0] = 0;
        bt_connected[i] = 0;
        bt_known[i] = 0;
        bt_seen[i] = 0;
    }
    if (connected) bt_connected[i] = 1;
    if (known) bt_known[i] = 1;
    if (name && name[0] && !bt_name_is_mac(name)) {
        if (!bt_names[i][0] || bt_name_is_mac(bt_names[i]))
            snprintf(bt_names[i], sizeof(bt_names[i]), "%s", name);
    }
}

static void bt_resolve_name(const char *mac, char *out, size_t n)
{
    out[0] = 0;
    char cmd[128], alias[96] = "", name[96] = "";
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    FILE *f = popen(cmd, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "Alias:", 6)) snprintf(alias, sizeof(alias), "%s", line + 6);
            else if (!strncmp(line, "Name:", 5)) snprintf(name, sizeof(name), "%s", line + 5);
        }
        pclose(f);
    }
    bt_trim(alias);
    bt_trim(name);
    const char *p = alias[0] ? alias : name;
    if (p[0] && !bt_name_is_mac(p)) snprintf(out, n, "%s", p);
}

static void bt_icon_of(const char *mac, char *out, size_t n)
{
    out[0] = 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Icon:", 5)) {
            snprintf(out, n, "%s", line + 5);
            bt_trim(out);
            break;
        }
    }
    pclose(f);
}

/* 1) BlueZ baza na disku: /var/lib/bluetooth/<ctrl>/<MAC>/info (+ cache) */
static void bt_from_disk(void)
{
    FILE *f = popen("find /var/lib/bluetooth -maxdepth 4 \\( -name info -o -path '*/cache/*' \\) -type f 2>/dev/null", "r");
    if (!f) return;
    char path[512];
    while (fgets(path, sizeof(path), f)) {
        bt_trim(path);
        if (!path[0]) continue;
        char mac[20] = "";
        const char *base = strrchr(path, '/');
        if (base && !strcmp(base, "/info")) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", path);
            char *slash = strrchr(tmp, '/');
            if (slash) {
                *slash = 0;
                char *macp = strrchr(tmp, '/');
                if (macp) snprintf(mac, sizeof(mac), "%s", macp + 1);
            }
        } else if (base) {
            snprintf(mac, sizeof(mac), "%s", base + 1);
        }
        if (strlen(mac) != 17) continue;

        char name[96] = "";
        FILE *g = fopen(path, "r");
        if (g) {
            char line[256];
            while (fgets(line, sizeof(line), g)) {
                if (!strncmp(line, "Name=", 5)) {
                    snprintf(name, sizeof(name), "%s", line + 5);
                    bt_trim(name);
                }
            }
            fclose(g);
        }
        bt_add(mac, name, 0, 1);
    }
    pclose(f);
}

/* 2) bluetoothctl devices [Trusted|Paired] */
static void bt_from_btctl(const char *filter, int known)
{
    char cmd[128];
    if (filter) snprintf(cmd, sizeof(cmd), "bluetoothctl devices %s 2>/dev/null", filter);
    else snprintf(cmd, sizeof(cmd), "bluetoothctl devices 2>/dev/null");
    FILE *f = popen(cmd, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char mac[20] = "", name[96] = "";
        if (sscanf(line, "Device %19s %95[^\n]", mac, name) < 1) continue;
        bt_trim(name);
        bt_add(mac, name, 0, known);
    }
    pclose(f);
}

/* 3) ispis poslednjeg skena */
static void bt_from_scanlog(void)
{
    FILE *f = fopen("/tmp/bt-scan.log", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *d = strstr(line, "Device ");
        if (!d) continue;
        char mac[20] = "", name[96] = "";
        if (sscanf(d, "Device %19s %95[^\n]", mac, name) != 2) continue;
        char *cut = strstr(name, "  ");
        if (cut) *cut = 0;
        bt_trim(name);
        bt_add(mac, name, 0, 0);
    }
    fclose(f);
}

/* 4) nas kes - da uredjaji ostanu vidljivi i posle reboota */
static void bt_from_cache(void)
{
    FILE *f = fopen(BT_CACHE, "r");
    if (!f) return;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char mac[20] = "", name[96] = "";
        if (sscanf(line, "%19s %95[^\n]", mac, name) < 1) continue;
        bt_trim(name);
        bt_add(mac, name, 0, 0);
    }
    fclose(f);
}

static void bt_save_cache(void)
{
    FILE *f = fopen(BT_CACHE, "w");
    if (!f) return;
    for (int i = 0; i < bt_count; i++) {
        if (!bt_names[i][0]) continue;
        fprintf(f, "%s\t%s\n", bt_macs[i], bt_names[i]);
    }
    fclose(f);
}

static void bt_swap(int a, int b)
{
    char tm[20], tn[96];
    snprintf(tm, sizeof(tm), "%s", bt_macs[a]);
    snprintf(tn, sizeof(tn), "%s", bt_names[a]);
    int tc = bt_connected[a], tk = bt_known[a];
    snprintf(bt_macs[a], sizeof(bt_macs[0]), "%s", bt_macs[b]);
    snprintf(bt_names[a], sizeof(bt_names[0]), "%s", bt_names[b]);
    bt_connected[a] = bt_connected[b];
    bt_known[a] = bt_known[b];
    snprintf(bt_macs[b], sizeof(bt_macs[0]), "%s", tm);
    snprintf(bt_names[b], sizeof(bt_names[0]), "%s", tn);
    bt_connected[b] = tc;
    bt_known[b] = tk;
}

static void bt_sort(void)
{
    for (int i = 1; i < bt_count; i++) {
        for (int j = i; j > 0; j--) {
            int a = j - 1, b = j, swap = 0;
            if (bt_connected[a] != bt_connected[b]) swap = bt_connected[b] > bt_connected[a];
            else if (bt_known[a] != bt_known[b]) swap = bt_known[b] > bt_known[a];
            else if (strcasecmp(bt_names[a], bt_names[b]) > 0) swap = 1;
            if (!swap) break;
            bt_swap(a, b);
        }
    }
}


static void bt_mark_seen(void)
{
    FILE *f = fopen("/tmp/bt-scan.log", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *d = strstr(line, "Device ");
        if (!d) continue;
        char mac[20] = "";
        if (sscanf(d, "Device %19s", mac) != 1) continue;
        int i = bt_find(mac);
        if (i >= 0) bt_seen[i] = 1;
    }
    fclose(f);
}

static void bt_load(void)
{
    bt_count = 0;

    char conn_mac[20] = "";
    FILE *ci = popen("bluetoothctl info 2>/dev/null", "r");
    if (ci) {
        char line[256];
        while (fgets(line, sizeof(line), ci)) {
            if (!strncmp(line, "Device ", 7)) {
                char mac[20] = "";
                if (sscanf(line, "Device %19s", mac) == 1)
                    snprintf(conn_mac, sizeof(conn_mac), "%s", mac);
            }
        }
        pclose(ci);
    }

    bt_from_disk();
    bt_from_btctl(NULL, 1);
    bt_from_btctl("Trusted", 1);
    bt_from_btctl("Paired", 1);
    bt_from_cache();
    bt_from_scanlog();
    bt_mark_seen();

    for (int i = 0; i < bt_count; i++) {
        if (conn_mac[0] && !strcmp(bt_macs[i], conn_mac)) bt_connected[i] = 1;
        if (!bt_names[i][0] || bt_name_is_mac(bt_names[i])) {
            char nm[96] = "";
            bt_resolve_name(bt_macs[i], nm, sizeof(nm));
            if (nm[0]) snprintf(bt_names[i], sizeof(bt_names[i]), "%s", nm);
        }
        if (!bt_names[i][0] || bt_name_is_mac(bt_names[i])) {
            char icon[32] = "";
            bt_icon_of(bt_macs[i], icon, sizeof(icon));
            if (icon[0])
                snprintf(bt_names[i], sizeof(bt_names[i]), "Unknown %s (%s)", icon, bt_macs[i] + 12);
            else
                snprintf(bt_names[i], sizeof(bt_names[i]), "Unknown (%s)", bt_macs[i] + 12);
        }
    }

    bt_sort();
    bt_save_cache();

    FILE *dbg = fopen("/tmp/bt-list.txt", "w");
    if (dbg) {
        for (int i = 0; i < bt_count; i++)
            fprintf(dbg, "%s\t%s\t%s%s\n", bt_macs[i], bt_names[i],
                    bt_connected[i] ? "connected" : "",
                    bt_known[i] ? "known" : "");
        fclose(dbg);
    }
}


/* font na panelu je ASCII - izbaci ne-ASCII (npr. kineska imena) */
static void bt_ascii(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 32 && c < 127) out[j++] = (char)c;
    }
    out[j] = 0;
    /* skini visestruke razmake */
    char *w = out, *r = out;
    int sp = 0;
    while (*r) {
        if (*r == ' ') { if (!sp) *w++ = ' '; sp = 1; }
        else { *w++ = *r; sp = 0; }
        r++;
    }
    *w = 0;
}

static void bt_fill(void)
{
    if (!bt_list) return;
    lv_obj_clean(bt_list);

    int conn = 0;
    for (int i = 0; i < bt_count; i++) if (bt_connected[i]) conn++;
    if (bt_status_lbl) {
        char b[200];
        snprintf(b, sizeof(b), "%d devices (%d connected)", bt_count, conn);
        lv_label_set_text(bt_status_lbl, b);
    }
    if (btn_bt_dis) {
        lv_obj_t *lb = lv_obj_get_child(btn_bt_dis, 0);
        if (lb) lv_obj_set_style_text_color(lb, lv_color_hex(conn ? COL_TEXT : COL_DIM), 0);
    }

    if (bt_count == 0) {
        lv_obj_t *l = lv_label_create(bt_list);
        lv_label_set_text(l, "No devices - tap Scan");
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        return;
    }

    for (int i = 0; i < bt_count; i++) {
        lv_obj_t *b = lv_button_create(bt_list);
        lv_obj_set_size(b, 760, 52);
        style_btn(b, i == bt_sel ? 0x1F3A38 : COL_PANEL, COL_TEXT, i == bt_sel);
        if (i == bt_sel) lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_t *l = lv_label_create(b);
        char t[160], ascii[96];
        bt_ascii(bt_names[i], ascii, sizeof(ascii));
        const char *mark = bt_connected[i] ? "  [connected]"
                        : (!bt_seen[i] ? "  [out of range]"
                                       : (bt_known[i] ? "  [known]" : ""));
        snprintf(t, sizeof(t), "%s%s", ascii[0] ? ascii : bt_names[i], mark);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l,
            lv_color_hex(bt_connected[i] ? COL_ACCENT : (bt_seen[i] ? COL_TEXT : COL_DIM)), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_add_event_cb(b, bt_pick_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void bt_reload_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    bt_load();
    bt_fill();
}

static void bt_scan_event(lv_event_t *e)
{
    (void)e;
    system("sh -c '(bluetoothctl --timeout 8 scan on > /tmp/bt-scan.log 2>&1) &'");
    lv_timer_create(bt_reload_cb, 7000, NULL);
    if (bt_status_lbl) lv_label_set_text(bt_status_lbl, "Scanning...");
}

static void bt_pick_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= bt_count) return;
    bt_sel = idx;
    snprintf(bt_sel_mac, sizeof(bt_sel_mac), "%s", bt_macs[idx]);
    bt_fill();
    if (bt_status_lbl) {
        char b[200];
        snprintf(b, sizeof(b), "Selected: %s", bt_names[idx]);
        lv_label_set_text(bt_status_lbl, b);
    }
}

static void bt_pair_poll_cb(lv_timer_t *t)
{
    static int ticks = 0;
    ticks++;
    int done = 0, ok = 0;
    FILE *f = fopen("/tmp/bt-pair.log", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "DONE", 4)) {
                done = 1;
                ok = strstr(line, "ok") != NULL;
            }
        }
        fclose(f);
    }
    if (done || ticks > 40) {
        lv_timer_delete(t);
        bt_pair_timer = NULL;
        ticks = 0;
        bt_load();
        bt_fill();
        if (bt_status_lbl) {
            if (!done) lv_label_set_text(bt_status_lbl, "Timed out");
            else if (ok) lv_label_set_text(bt_status_lbl, "Paired");
            else lv_label_set_text(bt_status_lbl, "Failed - try Scan");
        }
    }
}

static void bt_connect_event(lv_event_t *e)
{
    (void)e;
    if (!bt_sel_mac[0]) {
        if (bt_status_lbl) lv_label_set_text(bt_status_lbl, "Select a device from the list");
        return;
    }
    char cmd[300];
    snprintf(cmd, sizeof(cmd),
             "sh -c '/opt/radio/bt-pair.sh %s >/dev/null 2>&1' &", bt_sel_mac);
    system(cmd);
    if (bt_status_lbl) lv_label_set_text(bt_status_lbl, "Pairing...");
    if (!bt_pair_timer) bt_pair_timer = lv_timer_create(bt_pair_poll_cb, 500, NULL);
}

static void bt_disconnect_event(lv_event_t *e)
{
    (void)e;
    char mac[20] = "";
    for (int i = 0; i < bt_count; i++)
        if (bt_connected[i]) { snprintf(mac, sizeof(mac), "%s", bt_macs[i]); break; }
    if (!mac[0]) {
        if (bt_status_lbl) lv_label_set_text(bt_status_lbl, "No device connected");
        return;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s >/dev/null 2>&1", mac);
    system(cmd);
    bt_load();
    bt_fill();
    if (bt_status_lbl) lv_label_set_text(bt_status_lbl, "Disconnected");
}

static void bt_back_event(lv_event_t *e) { (void)e; nav_back(); }

static void bt_event(lv_event_t *e)
{
    (void)e;
    bt_load();
    bt_fill();
    nav_to(scr_bt);
}

static void build_bt(void)
{
    scr_bt = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_bt, lv_color_hex(COL_BG), 0);
    apply_bg(scr_bt);
    lv_obj_set_style_pad_all(scr_bt, 0, 0);
    lv_obj_clear_flag(scr_bt, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_label_create(scr_bt);
    lv_obj_set_pos(h, 24, 16);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_label_set_text(h, "BLUETOOTH");

    bt_status_lbl = lv_label_create(scr_bt);
    lv_obj_set_pos(bt_status_lbl, 24, 50);
    lv_obj_set_width(bt_status_lbl, 480);
    lv_label_set_long_mode(bt_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(bt_status_lbl, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(bt_status_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(bt_status_lbl, "");

    mk_btn(scr_bt, 268, 16, 118, 44, "Scan", bt_scan_event, NULL, 0);
    mk_btn(scr_bt, 394, 16, 112, 44, "Connect", bt_connect_event, NULL, 1);
    btn_bt_dis = mk_btn(scr_bt, 514, 16, 118, 44, "Disconnect", bt_disconnect_event, NULL, 0);
    mk_btn(scr_bt, 640, 16, 116, 44, "Back", bt_back_event, NULL, 0);

    bt_list = lv_obj_create(scr_bt);
    lv_obj_set_pos(bt_list, 16, 84);
    lv_obj_set_size(bt_list, 768, 382);
    lv_obj_set_style_bg_color(bt_list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(bt_list, 0, 0);
    lv_obj_set_style_pad_all(bt_list, 0, 0);
    lv_obj_set_style_pad_row(bt_list, 8, 0);
    lv_obj_set_flex_flow(bt_list, LV_FLEX_FLOW_COLUMN);
}

static void build_settings(void)
{
    scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_settings, lv_color_hex(COL_BG), 0);
    apply_bg(scr_settings);
    lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_label_create(scr_settings);
    lv_obj_set_pos(h, 24, 21);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_label_set_text(h, "Settings");

    lv_obj_t *i = lv_label_create(scr_settings);
    lv_obj_set_pos(i, 24, 80);
    lv_obj_set_style_text_color(i, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(i, &lv_font_montserrat_20, 0);
    lv_label_set_text(i, "IR - Internet Radio\nMPD + PCM5102 + Samba NAS\nVersion 0.1");

    mk_btn(scr_settings, 24, 170, 300, 60, LV_SYMBOL_WIFI "   WiFi", wifi_event, NULL, 0);
    mk_btn(scr_settings, 344, 170, 300, 60, LV_SYMBOL_BLUETOOTH "   Bluetooth", bt_event, NULL, 0);
    mk_btn(scr_settings, 654, 16, 122, 44, "Back", back_event, NULL, 0);
}

static void ui_timer(lv_timer_t *t)
{
    (void)t;
    static int tick = 0;
    mpd_status_t st;

    if (!g_connected) {
        if (mpd_connect(g_mpd, g_mpd->host, g_mpd->port) == 0) {
            g_connected = 1;
        } else {
            return;
        }
    }

    if (mpd_get_status(g_mpd, &st) != 0) {
        g_connected = 0;
        if (scr_splash && lv_screen_active() == scr_splash) {
            static int splash_fail = 0;
            if (++splash_fail > 16) nav_home();   /* ~8 s bez MPD */
        }
        return;
    }

    /* splash -> player: MPD je odgovorio (osim u testu: OIR_SPLASH_HOLD=1) */
    if (scr_splash && lv_screen_active() == scr_splash) {
        if (!getenv("OIR_SPLASH_HOLD")) nav_home();
        return;
    }

    if (slider_vol && !vol_drag) {
        lv_slider_set_value(slider_vol, st.volume < 0 ? 0 : st.volume, LV_ANIM_OFF);
        char b[16]; snprintf(b, sizeof(b), "%d%%", st.volume);
        lv_label_set_text(lbl_vol, b);
    }

    bt_mode = pulse_default_is_bt();
    apply_out_ui(bt_mode);
    if (slider_vol_menu && !vol_drag) {
        lv_slider_set_value(slider_vol_menu, st.volume < 0 ? 0 : st.volume, LV_ANIM_OFF);
        if (lbl_vol_menu) {
            char mb[16];
            snprintf(mb, sizeof(mb), "%d%%", st.volume < 0 ? 0 : st.volume);
            lv_label_set_text(lbl_vol_menu, mb);
        }
    }
    if (lbl_vol) {
        char vb[24];
        snprintf(vb, sizeof(vb), "Vol: %d%%", st.volume < 0 ? 0 : st.volume);
        lv_label_set_text(lbl_vol, vb);
    }
    if (lbl_clock) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char cb[16];
        snprintf(cb, sizeof(cb), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        lv_label_set_text(lbl_clock, cb);
    }
    static int last_playing = -1;
    int is_playing = (st.playing == 1);
    if (is_playing != last_playing) {
        last_playing = is_playing;
        if (img_play) lv_image_set_src(img_play, is_playing ? &icon_pause : &icon_play);
    }

    char l1[256] = "", l2[256] = "", url[512] = ""; int is_stream = 0;
    if (mpd_current_now(g_mpd, l1, sizeof(l1), l2, sizeof(l2), url, sizeof(url), &is_stream) == 0) {
        char title[256] = "", artist[256] = "", album[256] = "", data[96] = "";
        char topline[256] = "";
        if (is_stream) {
            char disp[256];
            station_display(url, l1, disp, sizeof(disp));
            const char *icy = (l2[0] && strcmp(l2, l1) && strcmp(l2, disp)) ? l2 : "";
            if (icy[0]) {
                const char *sep = strstr(icy, " - ");
                if (sep) {
                    snprintf(artist, sizeof(artist), "%.*s", (int)(sep - icy), icy);
                    snprintf(title, sizeof(title), "%s", sep + 3);
                } else {
                    snprintf(title, sizeof(title), "%s", icy);
                }
            } else {
                snprintf(title, sizeof(title), "LIVE");
            }
            album[0] = 0;                 /* stream: bez album reda */
            fmt_audio_line(&st, "STREAM", data, sizeof(data));
            snprintf(topline, sizeof(topline), "%s", disp);
            set_cover_for_url(url, l1);
            cur_art_url[0] = 0;   /* sledeci lokalni fajl mora ponovo da ucita cover */
            lv_bar_set_value(bar_prog, 0, LV_ANIM_OFF);
            lv_label_set_text(lbl_elapsed, "");
            lv_label_set_text(lbl_total, "");
        } else {
            split_artist_album(l1, artist, sizeof(artist), album, sizeof(album));
            snprintf(title, sizeof(title), "%s", l2);
            fmt_audio_line(&st, codec_for_url(url), data, sizeof(data));
            snprintf(topline, sizeof(topline), "%s", artist);
            if (strcmp(url, cur_art_url) != 0) {
                snprintf(cur_art_url, sizeof(cur_art_url), "%s", url);
                if (cover_fetch_file_rgb565("/mnt/nas", url, cover_buf, COVER_W, COVER_H) == 0) {
                    lv_obj_invalidate(canvas_cover);
                } else {
                    cover_clear();
                    lv_obj_invalidate(canvas_cover);
                }
            }
            if (st.duration > 0) {
                char tb[16];
                lv_bar_set_value(bar_prog, (int)((long)st.elapsed * 1000 / st.duration), LV_ANIM_OFF);
                fmt_mmss(st.elapsed, tb, sizeof(tb));
                lv_label_set_text(lbl_elapsed, tb);
                fmt_mmss(st.duration, tb, sizeof(tb));
                lv_label_set_text(lbl_total, tb);
            } else {
                lv_bar_set_value(bar_prog, 0, LV_ANIM_OFF);
                lv_label_set_text(lbl_elapsed, "");
                lv_label_set_text(lbl_total, "");
            }
        }
        lv_label_set_text(lbl_top, topline);
        lv_label_set_text(lbl_title, title);
        /* iste pozicije u oba rezima:
           stream: stanica / izvodjac / naslov
           lokal : izvodjac / album / naslov */
        lv_label_set_text(lbl_sub, is_stream ? artist : album);
        lv_label_set_text(lbl_src, is_stream ? "RADIO STREAM" : "LOCAL STREAM");
        lv_label_set_text(lbl_album, "");   /* album je u drugoj liniji */
        lv_label_set_text(lbl_data, data);
        snprintf(cur_url, sizeof(cur_url), "%s", url);
        if (is_stream) {
            int si = station_index_for_url(url);
            if (si >= 0 && si != active_station) {
                active_station = si;
                last_station = si;
                last_station_save(si);
                stations_refresh();
                if (lv_screen_active() == scr_stations) stations_center_active();
            }
        } else if (active_station != -1) {
            active_station = -1;
            stations_refresh();
        }
    }

    (void)tick;
}

/* javi oir-splash-u tek kad je prvi frejm stvarno nacrtan (flush), ne pre */
static void first_flush_cb(lv_event_t *e)
{
    (void)e;
    static int done = 0;
    if (done) return;
    done = 1;
    FILE *f = fopen("/run/oir-ui-ready", "w");
    if (f) fclose(f);
}

void ui_pi_init(mpd_t *mpd, int mpd_connected)
{
    g_mpd = mpd;
    g_connected = mpd_connected;
    vol_drag = 0;

    bg_init();
    splash_init();
    last_station_load();
    build_splash();
    build_player();
    build_menu();
    build_queue();
    build_playlists();
    build_stations();
    build_settings();
    build_local();
    build_wifi();
    build_bt();
    const char *start = getenv("OIR_START");
    lv_obj_t *first = splash_ready ? scr_splash : scr_player;
    if (start && !strcmp(start, "splash")) first = scr_splash;
    else if (start && !strcmp(start, "queue")) { queue_fill(); first = scr_queue; }
    else if (start && !strcmp(start, "playlists")) { pl_fill(); first = scr_pl; }
    else if (start && !strcmp(start, "menu")) first = scr_menu;
    else if (start && !strcmp(start, "local")) first = scr_local;
    else if (start && !strcmp(start, "wifi")) { wifi_scan_now(); wifi_fill(); first = scr_wifi; }
    else if (start && !strcmp(start, "wifipass")) {
        snprintf(wifi_pick, sizeof(wifi_pick), "%s", "TestNet");
        lv_label_set_text(wifi_pass_lbl, "Password for: TestNet");
        first = scr_wifi_pass;
    }
    else if (start && !strcmp(start, "settings")) first = scr_settings;
    else if (start && !strcmp(start, "stations")) { stations_refresh(); lv_timer_create(stations_center_cb, 400, NULL); first = scr_stations; }
    else if (start && !strcmp(start, "bt")) { bt_load(); bt_fill(); first = scr_bt; }
    lv_screen_load(first);
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_add_event_cb(disp, first_flush_cb, LV_EVENT_FLUSH_FINISH, NULL);
    lv_timer_create(ui_timer, 500, NULL);
}
