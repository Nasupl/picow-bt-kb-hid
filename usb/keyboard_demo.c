#include "keyboard_demo.h"

#include <stdbool.h>
#include <stddef.h>

#include "hid_keyboard.h"
#include "pico/stdlib.h"
#include "tusb.h"

typedef struct {
    uint8_t modifier;
    uint8_t keycode;
} keyboard_demo_key_t;

static keyboard_demo_key_t const demo_keys[] = {
    {0, HID_KEY_A}, {0, HID_KEY_B}, {0, HID_KEY_C},
    {KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_A},
    {KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_B},
    {KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_C},
    {KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_A},
    {0, HID_KEY_ENTER},
};

void keyboard_demo_task(void) {
    static bool mounted;
    static bool completed;
    static size_t key_index;
    static uint32_t next_key_ms;

    if (completed || !tud_mounted()) {
        return;
    }
    if (!mounted) {
        mounted = true;
        next_key_ms = to_ms_since_boot(get_absolute_time()) + 5000;
    }
    if ((int32_t) (to_ms_since_boot(get_absolute_time()) - next_key_ms) < 0 ||
        !tud_hid_ready()) {
        return;
    }

    keyboard_type(demo_keys[key_index].modifier, demo_keys[key_index].keycode);
    key_index++;
    completed = key_index == sizeof(demo_keys) / sizeof(demo_keys[0]);
    next_key_ms = to_ms_since_boot(get_absolute_time()) + 50;
}
