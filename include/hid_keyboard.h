#ifndef HID_KEYBOARD_H
#define HID_KEYBOARD_H

#include <stdint.h>

// Queue a complete USB boot-keyboard report. keycodes must contain six slots.
void keyboard_send_report(uint8_t modifier, const uint8_t keycodes[6]);
void keyboard_press(uint8_t modifier, uint8_t keycode);
void keyboard_release(void);
// Drop queued stale states and make an all-keys-released report the next state.
void keyboard_release_all(void);
void keyboard_type(uint8_t modifier, uint8_t keycode);

// Services TinyUSB and the USB-only API demonstration.
void hid_keyboard_task(void);

#endif
