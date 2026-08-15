#ifndef HID_REPORT_MAP_H
#define HID_REPORT_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_REPORT_MAP_MAX_FIELDS 256

typedef enum {
    HID_REPORT_FIELD_KEYBOARD_VARIABLE,
    HID_REPORT_FIELD_KEYBOARD_ARRAY,
    HID_REPORT_FIELD_CONSUMER_VARIABLE,
    HID_REPORT_FIELD_CONSUMER_ARRAY,
} hid_report_field_kind_t;

typedef struct {
    hid_report_field_kind_t kind;
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    uint16_t usage;
    uint16_t usage_minimum;
    uint16_t usage_maximum;
} hid_report_field_t;

typedef struct {
    bool valid;
    bool uses_report_ids;
    size_t field_count;
    hid_report_field_t fields[HID_REPORT_MAP_MAX_FIELDS];
} hid_report_map_t;

typedef struct {
    uint8_t modifier;
    uint8_t keycodes[6];
    uint16_t consumer_usage;
    bool keyboard_present;
    bool consumer_present;
    bool keyboard_rollover;
} hid_report_translation_t;

bool hid_report_map_compile(hid_report_map_t *map, const uint8_t *descriptor,
                            size_t length);
bool hid_report_map_translate(const hid_report_map_t *map,
                              const uint8_t *report, size_t length,
                              hid_report_translation_t *translation);

#endif
