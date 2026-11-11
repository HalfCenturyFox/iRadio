/* mpd_client.h - minimalni MPD klijent (line-based protokol preko TCP) */
#ifndef MPD_CLIENT_H
#define MPD_CLIENT_H

#include <stddef.h>

typedef struct {
    int  fd;
    char host[64];
    int  port;
} mpd_t;

typedef struct {
    int playing;   /* 0 = stop, 1 = play, 2 = pause */
    int volume;    /* 0..100, -1 ako nepoznato */
    int elapsed;
    int duration;
    int song;      /* pozicija trenutne pesme u queue-u, -1 ako nema */
    int queue_len; /* duzina queue-a */
    int audio_rate;/* Hz iz status linije "audio: 44100:16:2" */
    int audio_bits;
    int audio_ch;
    int bitrate;   /* kbps, 0 ako MPD ne prijavi */
} mpd_status_t;

int  mpd_connect(mpd_t *m, const char *host, int port);
void mpd_close(mpd_t *m);
int  mpd_is_connected(mpd_t *m);
int  mpd_cmd(mpd_t *m, const char *cmd);
int  mpd_cmd_read(mpd_t *m, const char *cmd, char *out, size_t out_n);
int  mpd_play_url(mpd_t *m, const char *url);
int  mpd_toggle(mpd_t *m);
int  mpd_stop(mpd_t *m);
int  mpd_volume_step(mpd_t *m, int delta);
int  mpd_setvol(mpd_t *m, int vol);
int  mpd_next(mpd_t *m);
int  mpd_previous(mpd_t *m);
int  mpd_get_status(mpd_t *m, mpd_status_t *st);
int  mpd_current_title(mpd_t *m, char *out, size_t n);
int  mpd_current_song(mpd_t *m, char *title, size_t tn, char *url, size_t un);
/* dve linije za player: za stream (l1=naziv stanice, l2=ICY pesma),
   za lokalni fajl (l1=izvodjac, l2=naziv pesme). url = file polje. */
int  mpd_current_now(mpd_t *m, char *l1, size_t n1, char *l2, size_t n2,
                     char *url, size_t un, int *is_stream);
int  mpd_queue(mpd_t *m, char *out, size_t n);
int  mpd_play_pos(mpd_t *m, int pos);

#endif /* MPD_CLIENT_H */
