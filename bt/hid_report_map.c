#include "hid_report_map.h"

#include <string.h>

#include "hid_report_descriptor.h"

#define HID_TYPE_MAIN 0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL 2
#define HID_MAIN_INPUT 8
#define HID_GLOBAL_USAGE_PAGE 0
#define HID_GLOBAL_REPORT_SIZE 7
#define HID_GLOBAL_REPORT_ID 8
#define HID_GLOBAL_REPORT_COUNT 9
#define HID_GLOBAL_PUSH 10
#define HID_GLOBAL_POP 11
#define HID_LOCAL_USAGE 0
#define HID_LOCAL_USAGE_MINIMUM 1
#define HID_LOCAL_USAGE_MAXIMUM 2
#define HID_USAGE_PAGE_KEYBOARD 0x07
#define HID_USAGE_PAGE_CONSUMER 0x0c
#define HID_GLOBAL_STACK_DEPTH 4
#define HID_LOCAL_USAGE_COUNT 32

typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
} map_global_t;

typedef struct {
    uint32_t page;
    uint16_t usage;
} map_usage_t;

static uint32_t item_value(const uint8_t *data, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint32_t) data[i] << (8 * i);
    return value;
}

static bool add_field(hid_report_map_t *map, hid_report_field_kind_t kind,
                      const map_global_t *global, uint16_t bit_offset,
                      uint16_t usage, uint16_t minimum, uint16_t maximum) {
    if (map->field_count == HID_REPORT_MAP_MAX_FIELDS ||
        global->report_size == 0 || global->report_size > 16) {
        return false;
    }
    hid_report_field_t *field = &map->fields[map->field_count++];
    field->kind = kind;
    field->report_id = global->report_id;
    field->bit_offset = bit_offset;
    field->bit_size = (uint8_t) global->report_size;
    field->usage = usage;
    field->usage_minimum = minimum;
    field->usage_maximum = maximum;
    return true;
}

static uint16_t *report_offset(uint16_t offsets[256], uint8_t report_id) {
    return &offsets[report_id];
}

bool hid_report_map_compile(hid_report_map_t *map, const uint8_t *descriptor,
                            size_t length) {
    if (map == NULL) return false;
    memset(map, 0, sizeof(*map));
    hid_report_descriptor_info_t info =
        hid_report_descriptor_parse(descriptor, length);
    if (!info.valid) return false;

    map_global_t global = {0};
    map_global_t stack[HID_GLOBAL_STACK_DEPTH];
    size_t stack_depth = 0;
    map_usage_t usages[HID_LOCAL_USAGE_COUNT];
    size_t usage_count = 0;
    bool have_minimum = false;
    map_usage_t minimum = {0};
    map_usage_t maximum = {0};
    uint16_t offsets[256] = {0};

    for (size_t offset = 0; offset < length;) {
        uint8_t prefix = descriptor[offset++];
        if (prefix == 0xfe) {
            size_t long_size = descriptor[offset];
            offset += 2 + long_size;
            continue;
        }
        size_t size_code = prefix & 3u;
        size_t data_size = size_code == 3 ? 4 : size_code;
        uint8_t type = (prefix >> 2) & 3u;
        uint8_t tag = prefix >> 4;
        uint32_t value = item_value(&descriptor[offset], data_size);
        offset += data_size;

        if (type == HID_TYPE_GLOBAL) {
            if (tag == HID_GLOBAL_USAGE_PAGE) global.usage_page = value;
            else if (tag == HID_GLOBAL_REPORT_SIZE) global.report_size = value;
            else if (tag == HID_GLOBAL_REPORT_COUNT) global.report_count = value;
            else if (tag == HID_GLOBAL_REPORT_ID) {
                global.report_id = (uint8_t) value;
                map->uses_report_ids = true;
            } else if (tag == HID_GLOBAL_PUSH) {
                stack[stack_depth++] = global;
            } else if (tag == HID_GLOBAL_POP) {
                global = stack[--stack_depth];
            }
            continue;
        }
        if (type == HID_TYPE_LOCAL &&
            (tag == HID_LOCAL_USAGE || tag == HID_LOCAL_USAGE_MINIMUM ||
             tag == HID_LOCAL_USAGE_MAXIMUM)) {
            map_usage_t usage = {
                .page = data_size == 4 ? value >> 16 : global.usage_page,
                .usage = (uint16_t) value,
            };
            if (tag == HID_LOCAL_USAGE) {
                if (usage_count == HID_LOCAL_USAGE_COUNT) return false;
                usages[usage_count++] = usage;
            } else if (tag == HID_LOCAL_USAGE_MINIMUM) {
                minimum = usage;
                have_minimum = true;
            } else if (tag == HID_LOCAL_USAGE_MAXIMUM) {
                maximum = usage;
            }
            continue;
        }
        if (type != HID_TYPE_MAIN) continue;

        if (tag == HID_MAIN_INPUT) {
            uint16_t *bit_offset = report_offset(offsets, global.report_id);
            uint32_t total_bits = global.report_size * global.report_count;
            if (total_bits > (uint32_t) UINT16_MAX - *bit_offset) return false;
            bool constant = (value & 1u) != 0;
            bool variable = (value & 2u) != 0;
            if (!constant) {
                if (variable) {
                    for (uint32_t i = 0; i < global.report_count; ++i) {
                        map_usage_t usage = {0};
                        if (i < usage_count) usage = usages[i];
                        else if (have_minimum && minimum.page == maximum.page &&
                                 minimum.usage + i <= maximum.usage) {
                            usage.page = minimum.page;
                            usage.usage = (uint16_t) (minimum.usage + i);
                        }
                        hid_report_field_kind_t kind;
                        if (usage.page == HID_USAGE_PAGE_KEYBOARD) {
                            kind = HID_REPORT_FIELD_KEYBOARD_VARIABLE;
                        } else if (usage.page == HID_USAGE_PAGE_CONSUMER) {
                            kind = HID_REPORT_FIELD_CONSUMER_VARIABLE;
                        } else {
                            continue;
                        }
                        if (!add_field(map, kind, &global,
                                       (uint16_t) (*bit_offset + i * global.report_size),
                                       usage.usage, 0, 0)) return false;
                    }
                } else {
                    if (!have_minimum) return false;
                    uint32_t page = have_minimum ? minimum.page :
                                    usage_count ? usages[0].page : 0;
                    uint16_t range_min = have_minimum ? minimum.usage : 0;
                    uint16_t range_max = have_minimum ? maximum.usage : UINT16_MAX;
                    hid_report_field_kind_t kind;
                    if (page == HID_USAGE_PAGE_KEYBOARD) {
                        kind = HID_REPORT_FIELD_KEYBOARD_ARRAY;
                    } else if (page == HID_USAGE_PAGE_CONSUMER) {
                        kind = HID_REPORT_FIELD_CONSUMER_ARRAY;
                    } else {
                        kind = HID_REPORT_FIELD_KEYBOARD_ARRAY;
                        page = 0;
                    }
                    if (page != 0) {
                        for (uint32_t i = 0; i < global.report_count; ++i) {
                            if (!add_field(map, kind, &global,
                                           (uint16_t) (*bit_offset + i * global.report_size),
                                           0, range_min, range_max)) return false;
                        }
                    }
                }
            }
            *bit_offset = (uint16_t) (*bit_offset + total_bits);
        }
        usage_count = 0;
        have_minimum = false;
        minimum = (map_usage_t) {0};
        maximum = (map_usage_t) {0};
    }
    map->valid = map->field_count != 0;
    return map->valid;
}

static bool read_bits(const uint8_t *data, size_t length, uint16_t offset,
                      uint8_t size, uint16_t *value) {
    if (size == 0 || size > 16 || (uint32_t) offset + size > length * 8u) {
        return false;
    }
    uint32_t result = 0;
    for (uint8_t bit = 0; bit < size; ++bit) {
        result |= ((data[(offset + bit) >> 3] >> ((offset + bit) & 7u)) & 1u)
                  << bit;
    }
    *value = (uint16_t) result;
    return true;
}

static void add_key(hid_report_translation_t *translation, uint16_t usage) {
    if (usage >= 0xe0 && usage <= 0xe7) {
        translation->modifier |= (uint8_t) (1u << (usage - 0xe0));
        translation->keyboard_present = true;
        return;
    }
    if (usage < 4 || usage > UINT8_MAX) return;
    translation->keyboard_present = true;
    for (size_t i = 0; i < sizeof(translation->keycodes); ++i) {
        if (translation->keycodes[i] == usage) return;
        if (translation->keycodes[i] == 0) {
            translation->keycodes[i] = (uint8_t) usage;
            return;
        }
    }
    translation->keyboard_rollover = true;
}

bool hid_report_map_translate(const hid_report_map_t *map,
                              const uint8_t *report, size_t length,
                              hid_report_translation_t *translation) {
    if (map == NULL || !map->valid || report == NULL || translation == NULL ||
        length == 0) return false;
    memset(translation, 0, sizeof(*translation));
    uint8_t report_id = map->uses_report_ids ? report[0] : 0;
    if (map->uses_report_ids) {
        ++report;
        --length;
    }
    bool matched = false;
    for (size_t i = 0; i < map->field_count; ++i) {
        const hid_report_field_t *field = &map->fields[i];
        if (field->report_id != report_id) continue;
        uint16_t value;
        if (!read_bits(report, length, field->bit_offset, field->bit_size,
                       &value)) return false;
        matched = true;
        if (field->kind == HID_REPORT_FIELD_KEYBOARD_VARIABLE) {
            translation->keyboard_present = true;
            if (value != 0) add_key(translation, field->usage);
        } else if (field->kind == HID_REPORT_FIELD_KEYBOARD_ARRAY) {
            translation->keyboard_present = true;
            if (value >= field->usage_minimum && value <= field->usage_maximum) {
                add_key(translation, value);
            }
        } else if (field->kind == HID_REPORT_FIELD_CONSUMER_VARIABLE) {
            translation->consumer_present = true;
            if (value != 0 && field->usage != 0) {
                translation->consumer_usage = field->usage;
            }
        } else {
            translation->consumer_present = true;
            if (value >= field->usage_minimum &&
                value <= field->usage_maximum && value != 0) {
                translation->consumer_usage = value;
            }
        }
    }
    return matched;
}
