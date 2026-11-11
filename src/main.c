/* main.c - Pi4 internet radio UI (LVGL 9 fbdev + evdev touch) */
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stations.h"
#include "mpd_client.h"
extern void ui_pi_init(mpd_t *mpd, int connected);

static uint32_t tick_get_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

int main(int argc, char **argv)
{
    const char *fb = "/dev/fb0";
    const char *touch = "/dev/input/event4";
    const char *mpdhost = MPD_DEFAULT_HOST;
    int mpdport = MPD_DEFAULT_PORT;
    int i;
    lv_display_t *disp;
    lv_indev_t *indev;
    mpd_t mpd;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fb") && i + 1 < argc)        fb = argv[++i];
        else if (!strcmp(argv[i], "--touch") && i + 1 < argc) touch = argv[++i];
        else if (!strcmp(argv[i], "--mpd") && i + 1 < argc)   mpdhost = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)  mpdport = atoi(argv[++i]);
    }

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    disp = lv_linux_fbdev_create();
    if (!disp) {
        fprintf(stderr, "radio-ui: fbdev display failed\n");
        return 1;
    }
    lv_linux_fbdev_set_file(disp, fb);
    lv_display_set_default(disp);

    indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
    if (!indev)
        fprintf(stderr, "radio-ui: touch %s unavailable (continuing)\n", touch);

    printf("radio-ui: fb=%s touch=%s mpd=%s:%d\n", fb, touch, mpdhost, mpdport);

    memset(&mpd, 0, sizeof(mpd));
    snprintf(mpd.host, sizeof(mpd.host), "%s", mpdhost);
    mpd.port = mpdport;
    if (mpd_connect(&mpd, mpdhost, mpdport) != 0)
        fprintf(stderr, "radio-ui: MPD %s:%d not reachable\n", mpdhost, mpdport);

    ui_pi_init(&mpd, mpd_is_connected(&mpd));

    for (;;) {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
