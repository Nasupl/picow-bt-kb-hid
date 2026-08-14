#ifndef BLUETOOTH_APP_H
#define BLUETOOTH_APP_H

#include <stdbool.h>
#include <stdint.h>

void bluetooth_init(void);
void bluetooth_task(void);
void bluetooth_set_keyboard_leds(uint8_t leds);

// Bridge reserved for a future BTstack HID Host report callback.
void bluetooth_forward_key_event(bool pressed, uint8_t modifier, uint8_t keycode);

#endif
