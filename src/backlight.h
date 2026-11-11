/* backlight.h - pozadinsko osvetljenje preko PWM sysfs (GPIO12) */
#ifndef BACKLIGHT_H
#define BACKLIGHT_H

/* chip: npr. "/sys/class/pwm/pwmchip0", channel: 0 */
int  backlight_init(const char *chip, int channel);
int  backlight_set_percent(int pct);   /* 0..100, vraca 0 ili -1 */
int  backlight_get_percent(void);
void backlight_close(void);

#endif /* BACKLIGHT_H */
