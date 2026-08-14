#include "hid_keyboard.h"

#include <string.h>

#include "keyboard_demo.h"
#include "bluetooth_app.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_serial.h"

#define KEYBOARD_REPORT_QUEUE_SIZE 64

typedef struct {
    uint8_t modifier;
    uint8_t keycodes[6];
} keyboard_report_t;

static keyboard_report_t report_queue[KEYBOARD_REPORT_QUEUE_SIZE];
static uint8_t report_queue_head;
static uint8_t report_queue_count;

void keyboard_send_report(uint8_t modifier, const uint8_t keycodes[6]) {
    if (report_queue_count == KEYBOARD_REPORT_QUEUE_SIZE) {
        // Preserve the newest physical state. A full queue indicates that USB
        // could not drain reports fast enough; discarding stale transitions is
        // safer than leaving the host with an old pressed-key state.
        report_queue_head = 0;
        report_queue_count = 0;
        usb_serial_printf("[USB] Report queue overflow; stale reports dropped\r\n");
    }

    uint8_t tail = (uint8_t) ((report_queue_head + report_queue_count) %
                              KEYBOARD_REPORT_QUEUE_SIZE);
    report_queue[tail].modifier = modifier;
    memcpy(report_queue[tail].keycodes, keycodes,
           sizeof(report_queue[tail].keycodes));
    report_queue_count++;
}

void keyboard_press(uint8_t modifier, uint8_t keycode) {
    uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
    keyboard_send_report(modifier, keycodes);
}

void keyboard_release(void) {
    uint8_t keycodes[6] = {0};
    keyboard_send_report(0, keycodes);
}

void keyboard_release_all(void) {
    report_queue_head = 0;
    report_queue_count = 0;
    keyboard_release();
}

void keyboard_type(uint8_t modifier, uint8_t keycode) {
    keyboard_press(modifier, keycode);
    sleep_ms(10);
    keyboard_release();
}

void hid_keyboard_task(void) {
    tud_task();
    usb_serial_task();

    if (report_queue_count && tud_hid_ready()) {
        keyboard_report_t *report = &report_queue[report_queue_head];
        if (tud_hid_keyboard_report(0, report->modifier, report->keycodes)) {
            usb_serial_printf(
                "[USB] TX mod=%02x keys=%02x,%02x,%02x,%02x,%02x,%02x\r\n",
                report->modifier,
                report->keycodes[0], report->keycodes[1],
                report->keycodes[2], report->keycodes[3],
                report->keycodes[4], report->keycodes[5]);
            report_queue_head = (uint8_t) ((report_queue_head + 1) %
                                           KEYBOARD_REPORT_QUEUE_SIZE);
            report_queue_count--;
        }
    }
#ifdef ENABLE_KEYBOARD_DEMO
    keyboard_demo_task();
#endif
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void) instance;
    (void) report_id;
    if (report_type == HID_REPORT_TYPE_OUTPUT && buffer != NULL &&
        bufsize >= 1) {
        uint8_t leds = buffer[0] & 0x1f;
        usb_serial_printf("[USB] LED RX bits=0x%02x\r\n", leds);
        bluetooth_set_keyboard_leds(leds);
    }
}
