#include "hid_report_map.h"

#include <string.h>

#include "hid_report_descriptor.h"

#define HID_TYPE_MAIN 0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL 2
#define HID_MAIN_INPUT 8
#define HID_GLOBAL_USAGE_PAGE 0
#define HID_GLOBAL_LOGICAL_MINIMUM 1
#define HID_GLOBAL_REPORT_SIZE 7
#define HID_GLOBAL_REPORT_ID 8
#define HID_GLOBAL_REPORT_COUNT 9
#define HID_GLOBAL_PUSH 10
#define HID_GLOBAL_POP 11
#define HID_LOCAL_USAGE 0
#define HID_LOCAL_USAGE_MINIMUM 1
#define HID_LOCAL_USAGE_MAXIMUM 2
#define HID_LOCAL_DELIMITER 10
#define HID_USAGE_PAGE_KEYBOARD 0x07
#define HID_USAGE_PAGE_CONSUMER 0x0c
#define HID_GLOBAL_STACK_DEPTH 4
#define HID_LOCAL_USAGE_COUNT 1024

typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    int32_t logical_minimum;
    uint8_t report_id;
} map_global_t;

typedef struct {
    uint32_t page;
    uint16_t usage;
} map_usage_t;

// Compilation runs synchronously in the BTstack context. Keep the expanded
// usage list in BSS rather than consuming the RP2040 main stack.
static map_usage_t local_usages[HID_LOCAL_USAGE_COUNT];

static uint32_t item_value(const uint8_t *data, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint32_t) data[i] << (8 * i);
    return value;
}

static int32_t signed_item_value(const uint8_t *data, size_t size) {
    uint32_t value = item_value(data, size);
    if (size == 1) return (int8_t) value;
    if (size == 2) return (int16_t) value;
    return (int32_t) value;
}

static bool add_field(hid_report_map_t *map, hid_report_field_kind_t kind,
                      const map_global_t *global, uint16_t bit_offset,
                      uint16_t usage, uint16_t minimum, uint16_t maximum,
                      int32_t selector_minimum, int32_t selector_maximum) {
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
    field->selector_minimum = selector_minimum;
    field->selector_maximum = selector_maximum;
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
    map_usage_t *usages = local_usages;
    size_t usage_count = 0;
    bool have_minimum = false;
    bool ignore_alternative_usages = false;
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
            else if (tag == HID_GLOBAL_LOGICAL_MINIMUM) {
                global.logical_minimum = signed_item_value(
                    &descriptor[offset - data_size], data_size);
            }
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
            if (ignore_alternative_usages) continue;
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
                if (have_minimum && minimum.page == maximum.page &&
                    minimum.usage <= maximum.usage) {
                    for (uint32_t item = minimum.usage;
                         item <= maximum.usage; ++item) {
                        if (usage_count == HID_LOCAL_USAGE_COUNT) return false;
                        usages[usage_count++] = (map_usage_t) {
                            .page = minimum.page,
                            .usage = (uint16_t) item,
                        };
                    }
                }
                have_minimum = false;
            }
            continue;
        }
        if (type == HID_TYPE_LOCAL && tag == HID_LOCAL_DELIMITER) {
            ignore_alternative_usages = value == 1;
            continue;
        }
        if (type != HID_TYPE_MAIN) continue;

        if (tag == HID_MAIN_INPUT) {
            uint16_t *bit_offset = report_offset(offsets, global.report_id);
            uint64_t total_bits =
                (uint64_t) global.report_size * global.report_count;
            if (total_bits > (uint32_t) UINT16_MAX - *bit_offset) return false;
            bool constant = (value & 1u) != 0;
            bool variable = (value & 2u) != 0;
            if (!constant) {
                if (variable) {
                    uint64_t emitted_count = 0;
                    size_t declared_count = usage_count;
                    if (declared_count > global.report_count) {
                        declared_count = global.report_count;
                    }
                    for (size_t i = 0; i < declared_count; ++i) {
                        emitted_count +=
                            usages[i].page == HID_USAGE_PAGE_KEYBOARD ||
                            usages[i].page == HID_USAGE_PAGE_CONSUMER;
                    }
                    bool repeated_relevant = usage_count != 0 &&
                        (usages[usage_count - 1].page ==
                             HID_USAGE_PAGE_KEYBOARD ||
                         usages[usage_count - 1].page ==
                             HID_USAGE_PAGE_CONSUMER);
                    if (global.report_count > usage_count &&
                        repeated_relevant) {
                        emitted_count += global.report_count - usage_count;
                    }
                    if (emitted_count >
                        HID_REPORT_MAP_MAX_FIELDS - map->field_count) {
                        return false;
                    }
                    uint32_t positions_to_scan = global.report_count;
                    if (global.report_count > usage_count &&
                        !repeated_relevant) {
                        positions_to_scan = (uint32_t) usage_count;
                    }
                    for (uint32_t i = 0; i < positions_to_scan; ++i) {
                        map_usage_t usage = {0};
                        if (i < usage_count) usage = usages[i];
                        else if (usage_count != 0) usage = usages[usage_count - 1];
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
                                       usage.usage, 0, 0, 0, 0)) return false;
                    }
                } else {
                    if (usage_count != 0 &&
                        global.report_count > HID_REPORT_MAP_MAX_FIELDS) {
                        return false;
                    }
                    for (uint32_t i = 0; i < global.report_count; ++i) {
                        uint16_t field_offset = (uint16_t) (
                            *bit_offset + i * global.report_size);
                        for (size_t u = 0; u < usage_count;) {
                            hid_report_field_kind_t kind;
                            if (usages[u].page == HID_USAGE_PAGE_KEYBOARD) {
                                kind = HID_REPORT_FIELD_KEYBOARD_ARRAY;
                            } else if (usages[u].page ==
                                       HID_USAGE_PAGE_CONSUMER) {
                                kind = HID_REPORT_FIELD_CONSUMER_ARRAY;
                            } else {
                                ++u;
                                continue;
                            }
                            size_t run_end = u;
                            while (run_end + 1 < usage_count &&
                                   usages[run_end + 1].page == usages[u].page &&
                                   usages[run_end + 1].usage ==
                                       usages[run_end].usage + 1) {
                                ++run_end;
                            }
                            int32_t selector_minimum =
                                global.logical_minimum + (int32_t) u;
                            if (!add_field(map, kind, &global,
                                           field_offset, 0, usages[u].usage,
                                           usages[run_end].usage,
                                           selector_minimum,
                                           selector_minimum +
                                               (int32_t) (run_end - u))) {
                                return false;
                            }
                            u = run_end + 1;
                        }
                    }
                }
            }
            *bit_offset = (uint16_t) (*bit_offset + (uint32_t) total_bits);
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

static int32_t decode_selector(uint16_t value, uint8_t bit_size,
                               int32_t logical_minimum) {
    if (logical_minimum < 0 && bit_size <= 16 &&
        (value & (1u << (bit_size - 1))) != 0) {
        return (int32_t) value | -(1 << bit_size);
    }
    return value;
}

static void add_key(hid_report_translation_t *translation, uint16_t usage) {
    if (usage >= 1 && usage <= 3) {
        translation->keyboard_present = true;
        translation->keyboard_rollover = true;
        return;
    }
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
            int32_t selector = decode_selector(
                value, field->bit_size, field->selector_minimum);
            if (selector >= field->selector_minimum &&
                selector <= field->selector_maximum) {
                add_key(translation, (uint16_t) (
                    field->usage_minimum +
                    (selector - field->selector_minimum)));
            }
        } else if (field->kind == HID_REPORT_FIELD_CONSUMER_VARIABLE) {
            translation->consumer_present = true;
            if (value != 0 && field->usage != 0) {
                translation->consumer_usage = field->usage;
            }
        } else {
            translation->consumer_present = true;
            int32_t selector = decode_selector(
                value, field->bit_size, field->selector_minimum);
            if (selector >= field->selector_minimum &&
                selector <= field->selector_maximum) {
                uint16_t mapped_usage = (uint16_t) (
                    field->usage_minimum +
                    (selector - field->selector_minimum));
                if (mapped_usage != 0) {
                    translation->consumer_usage = mapped_usage;
                }
            }
        }
    }
    return matched;
}
