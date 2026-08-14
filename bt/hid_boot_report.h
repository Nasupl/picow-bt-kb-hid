#ifndef HID_BOOT_REPORT_H
#define HID_BOOT_REPORT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HID_BOOT_PARSE_OK,
    HID_BOOT_PARSE_UNSUPPORTED,
    HID_BOOT_PARSE_INVALID,
} hid_boot_parse_result_t;

typedef struct {
    uint8_t report_id;
    uint8_t modifier;
    uint8_t keycodes[6];
} hid_boot_keyboard_report_t;

bool hid_boot_keyboard_report_valid(const uint8_t report[8]);
hid_boot_parse_result_t hid_boot_keyboard_parse_input(
    const uint8_t *packet, uint16_t size, hid_boot_keyboard_report_t *report);
uint8_t hid_boot_keyboard_output_packet(uint8_t leds, uint8_t packet[2]);

#endif
