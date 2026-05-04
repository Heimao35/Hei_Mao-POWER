#ifndef LED_RGB_H
#define LED_RGB_H

#include <stdbool.h>

/** Init RMT + WS2812 on GPIO38 (call once before using LEDs). */
void led_rgb_init(void);

/** Start/stop rainbow animation on both LEDs. When off, LEDs are turned off. */
void led_rgb_set_rainbow_running(bool on);

bool led_rgb_is_rainbow_running(void);

#endif
