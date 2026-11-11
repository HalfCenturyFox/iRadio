/* mpd_client.c - minimalni MPD klijent. Bez libmpdclient zavisnosti:
 * MPD protokol je tekstualan ("komanda\n" -> linije "kljuc: vrednost" -> "OK\n").
 * Dovoljno za radio: status, currentsong, add/play/stop/setvol.
 */
#include "mpd_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

#define MPD_BUF 32768

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* cita jednu liniju; vraca duzinu, 0 na EOF, -1 na gresci */
static int read_line(int fd, char *buf, size_t n)
{
    size_t i = 0;
    while (i + 1 < n) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (c == '\n') {
            buf[i] = '\0';
            return (int)i;
        }
        buf[i++] = c;
    }
    buf[n - 1] = '\0';
    return (int)(n - 1);
}

int mpd_is_connected(mpd_t *m)
{
    return m && m->fd >= 0;
}

int mpd_connect(mpd_t *m, const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *it;
    char portstr[16];
    char line[256];

    memset(m, 0, sizeof(*m));
    m->fd = -1;
    snprintf(m->host, sizeof(m->host), "%s", host);
    m->port = port;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (it = res; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            m->fd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(res);

    if (m->fd < 0)
        return -1;

    {
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        (void)setsockopt(m->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(m->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* MPD pozdrav: "OK MPD x.y.z" */
    if (read_line(m->fd, line, sizeof(line)) <= 0 || strncmp(line, "OK MPD", 6) != 0) {
        close(m->fd);
        m->fd = -1;
        return -1;
    }
    return 0;
}

void mpd_close(mpd_t *m)
{
    if (m && m->fd >= 0) {
        close(m->fd);
        m->fd = -1;
    }
}

int mpd_cmd(mpd_t *m, const char *cmd)
{
    return mpd_cmd_read(m, cmd, NULL, 0);
}

int mpd_cmd_read(mpd_t *m, const char *cmd, char *out, size_t out_n)
{
    char buf[1024];
    char line[MPD_BUF];
    size_t used = 0;

    if (!mpd_is_connected(m))
        return -1;
    if (out && out_n)
        out[0] = '\0';

    snprintf(buf, sizeof(buf), "%s\n", cmd);
    if (send_all(m->fd, buf, strlen(buf)) < 0)
        return -1;

    for (;;) {
        int len = read_line(m->fd, line, sizeof(line));
        if (len <= 0)
            return -1;
        if (strcmp(line, "OK") == 0)
            return 0;
        if (strncmp(line, "ACK", 3) == 0)
            return -1;
        if (out && out_n > 1) {
            size_t need = (size_t)len + 1;
            if (used + need + 1 < out_n) {
                memcpy(out + used, line, need);
                used += need;
                out[used - 1] = '\n';
                out[used] = '\0';
            }
        }
    }
}

/* MPD zahteva navodnike i escape za " i \ u argumentima */
static void quote_arg(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (o + 1 < n) out[o++] = '"';
    for (; *in && o + 2 < n; in++) {
        if (*in == '"' || *in == '\\') {
            out[o++] = '\\';
        }
        out[o++] = *in;
    }
    if (o + 1 < n) out[o++] = '"';
    out[o] = '\0';
}

int mpd_play_url(mpd_t *m, const char *url)
{
    char quoted[1024];
    char cmd[1100];

    quote_arg(url, quoted, sizeof(quoted));
    (void)mpd_cmd(m, "clear");
    snprintf(cmd, sizeof(cmd), "add %s", quoted);
    if (mpd_cmd(m, cmd) < 0)
        return -1;
    return mpd_cmd(m, "play");
}

int mpd_next(mpd_t *m) { return mpd_cmd(m, "next"); }
int mpd_previous(mpd_t *m) { return mpd_cmd(m, "previous"); }

int mpd_setvol(mpd_t *m, int vol)
{
    char cmd[32];
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    snprintf(cmd, sizeof(cmd), "setvol %d", vol);
    return mpd_cmd(m, cmd);
}

int mpd_stop(mpd_t *m)
{
    return mpd_cmd(m, "stop");
}

static const char *find_value(const char *reply, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    const char *p = reply;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            size_t vlen = len - klen - 1;
            while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
            if (vlen >= n)
                vlen = n - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return out;
        }
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

int mpd_get_status(mpd_t *m, mpd_status_t *st)
{
    char reply[MPD_BUF];
    char v[64];

    memset(st, 0, sizeof(*st));
    st->volume = -1;
    st->song = -1;
    st->queue_len = 0;

    if (mpd_cmd_read(m, "status", reply, sizeof(reply)) < 0)
        return -1;

    if (find_value(reply, "state", v, sizeof(v))) {
        if (strcmp(v, "play") == 0)       st->playing = 1;
        else if (strcmp(v, "pause") == 0) st->playing = 2;
        else                              st->playing = 0;
    }
    if (find_value(reply, "volume", v, sizeof(v)))
        st->volume = atoi(v);
    if (find_value(reply, "elapsed", v, sizeof(v)))
        st->elapsed = (int)atof(v);
    if (find_value(reply, "duration", v, sizeof(v)))
        st->duration = (int)atof(v);
    if (find_value(reply, "song", v, sizeof(v)))
        st->song = atoi(v);
    if (find_value(reply, "playlistlength", v, sizeof(v)))
        st->queue_len = atoi(v);
    if (find_value(reply, "audio", v, sizeof(v)))
        sscanf(v, "%d:%d:%d", &st->audio_rate, &st->audio_bits, &st->audio_ch);
    if (find_value(reply, "bitrate", v, sizeof(v)))
        st->bitrate = atoi(v);
    return 0;
}

int mpd_queue(mpd_t *m, char *out, size_t n)
{
    return mpd_cmd_read(m, "playlistinfo", out, n);
}

int mpd_play_pos(mpd_t *m, int pos)
{
    char cmd[32];

    snprintf(cmd, sizeof(cmd), "play %d", pos);
    return mpd_cmd(m, cmd);
}

int mpd_toggle(mpd_t *m)
{
    mpd_status_t st;

    if (mpd_get_status(m, &st) < 0)
        return -1;
    if (st.playing == 1)
        return mpd_cmd(m, "pause 1");
    return mpd_cmd(m, "play");
}

int mpd_volume_step(mpd_t *m, int delta)
{
    mpd_status_t st;
    int target;
    char cmd[32];

    if (mpd_get_status(m, &st) < 0)
        return -1;
    if (st.volume < 0)
        st.volume = 50;
    target = st.volume + delta;
    if (target < 0)   target = 0;
    if (target > 100) target = 100;

    snprintf(cmd, sizeof(cmd), "setvol %d", target);
    return mpd_cmd(m, cmd);
}

int mpd_current_song(mpd_t *m, char *title, size_t tn, char *url, size_t un)
{
    char reply[MPD_BUF];
    char v[512];

    if (title && tn)
        title[0] = '\0';
    if (url && un)
        url[0] = '\0';
    if (mpd_cmd_read(m, "currentsong", reply, sizeof(reply)) < 0)
        return -1;

    if (url && un && find_value(reply, "file", v, sizeof(v)))
        snprintf(url, un, "%s", v);
    if (title && tn) {
        if (find_value(reply, "Title", v, sizeof(v)))       snprintf(title, tn, "%s", v);
        else if (find_value(reply, "Name", v, sizeof(v)))   snprintf(title, tn, "%s", v);
        else if (find_value(reply, "file", v, sizeof(v)))   snprintf(title, tn, "%s", v);
    }
    return 0;
}

static int is_stream_url(const char *u)
{
    return u && (!strncmp(u, "http", 4) || !strncmp(u, "icy", 3) || strstr(u, "://") != NULL);
}

int mpd_current_now(mpd_t *m, char *l1, size_t n1, char *l2, size_t n2,
                    char *url, size_t un, int *is_stream)
{
    char reply[MPD_BUF];
    char f[512] = "", artist[256] = "", alb[256] = "", album[256] = "", title[512] = "", name[256] = "";

    if (l1 && n1) l1[0] = '\0';
    if (l2 && n2) l2[0] = '\0';
    if (url && un) url[0] = '\0';
    if (is_stream) *is_stream = 0;

    if (mpd_cmd_read(m, "currentsong", reply, sizeof(reply)) < 0)
        return -1;

    if (find_value(reply, "file", f, sizeof(f)) == 0) f[0] = '\0';
    if (find_value(reply, "Artist", artist, sizeof(artist)) == 0) artist[0] = '\0';
    if (find_value(reply, "AlbumArtist", alb, sizeof(alb)) == 0) alb[0] = '\0';
    if (find_value(reply, "Album", album, sizeof(album)) == 0) album[0] = '\0';
    if (find_value(reply, "Title", title, sizeof(title)) == 0) title[0] = '\0';
    if (find_value(reply, "Name", name, sizeof(name)) == 0) name[0] = '\0';

    if (url && un) snprintf(url, un, "%s", f);

    if (is_stream_url(f)) {
        if (is_stream) *is_stream = 1;
        if (l1 && n1) snprintf(l1, n1, "%s", name[0] ? name : (title[0] ? title : "Radio"));
        if (l2 && n2) {
            if (title[0] && (!l1 || strcmp(title, l1) != 0)) snprintf(l2, n2, "%s", title);
            else l2[0] = '\0';
        }
    } else {
        const char *a = artist[0] ? artist : alb;
        if (l1 && n1) {
            if (a[0] && album[0]) snprintf(l1, n1, "%s - %s", a, album);
            else snprintf(l1, n1, "%s", a[0] ? a : (f[0] ? "Unknown artist" : ""));
        }
        if (l2 && n2) {
            if (title[0]) snprintf(l2, n2, "%s", title);
            else {
                const char *sl = strrchr(f, '/');
                snprintf(l2, n2, "%s", sl ? sl + 1 : f);
            }
        }
    }
    return 0;
}

int mpd_current_title(mpd_t *m, char *out, size_t n)
{
    char reply[MPD_BUF];
    char v[256];

    if (out && n)
        out[0] = '\0';
    if (mpd_cmd_read(m, "currentsong", reply, sizeof(reply)) < 0)
        return -1;

    /* radio streamovi obicno daju Title (ICY) ili Name; file je fallback */
    if (find_value(reply, "Title", v, sizeof(v))) {
        snprintf(out, n, "%s", v);
        return 0;
    }
    if (find_value(reply, "Name", v, sizeof(v))) {
        snprintf(out, n, "%s", v);
        return 0;
    }
    if (find_value(reply, "file", v, sizeof(v))) {
        snprintf(out, n, "%s", v);
        return 0;
    }
    return 0;
}
