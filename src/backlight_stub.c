/* backlight_stub.c - Pi4 UI: no PWM backlight control (DSI panel handles it) */
#include "backlight.h"

int backlight_init(const char *chip, int channel) { (void)chip; (void)channel; return 0; }
int backlight_set_percent(int pct) { (void)pct; return 0; }
int backlight_get_percent(void) { return 80; }
void backlight_close(void) {}
