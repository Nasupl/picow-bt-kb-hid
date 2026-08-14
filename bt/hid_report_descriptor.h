#ifndef HID_REPORT_DESCRIPTOR_H
#define HID_REPORT_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool valid;
    bool has_keyboard;
    bool has_consumer_control;
    bool has_nkro_keyboard;
    bool uses_report_ids;
} hid_report_descriptor_info_t;

hid_report_descriptor_info_t hid_report_descriptor_parse(
    const uint8_t *descriptor, size_t length);

#endif
