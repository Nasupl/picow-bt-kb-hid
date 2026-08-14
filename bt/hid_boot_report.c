#include "hid_boot_report.h"

#include <stddef.h>
#include <string.h>

bool hid_boot_keyboard_report_valid(const uint8_t report[8]) {
    if (report[1] != 0) {
        return false;
    }

    bool contains_error_usage = false;
    bool all_same_error_usage = report[2] >= 1 && report[2] <= 3;
    for (size_t i = 2; i < 8; ++i) {
        uint8_t keycode = report[i];
        if (keycode >= 1 && keycode <= 3) {
            contains_error_usage = true;
        }
        if (keycode != report[2]) {
            all_same_error_usage = false;
        }
    }

    // HID error usages 1..3 are valid only as a six-slot rollover/error
    // report. Mixed values are malformed and may be device management data.
    return !contains_error_usage || all_same_error_usage;
}

hid_boot_parse_result_t hid_boot_keyboard_parse_input(
    const uint8_t *packet, uint16_t size, hid_boot_keyboard_report_t *report) {
    if (packet == NULL || report == NULL || (size != 9 && size != 10)) {
        return HID_BOOT_PARSE_UNSUPPORTED;
    }
    // HIDP DATA (0xa0) | INPUT (0x01).
    if (packet[0] != 0xa1) {
        return HID_BOOT_PARSE_UNSUPPORTED;
    }

    const uint8_t *boot_report;
    if (size == 10) {
        report->report_id = packet[1];
        boot_report = &packet[2];
    } else {
        report->report_id = 0;
        boot_report = &packet[1];
    }
    if (!hid_boot_keyboard_report_valid(boot_report)) {
        return HID_BOOT_PARSE_INVALID;
    }

    report->modifier = boot_report[0];
    memcpy(report->keycodes, &boot_report[2], sizeof(report->keycodes));
    return HID_BOOT_PARSE_OK;
}

uint8_t hid_boot_keyboard_output_packet(uint8_t leds, uint8_t packet[2]) {
    // HIDP DATA | OUTPUT followed by the one-byte Boot Keyboard LED report.
    packet[0] = 0xa2;
    packet[1] = leds & 0x1f;
    return 2;
}
