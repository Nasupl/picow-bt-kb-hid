#include "hid_report_descriptor.h"

#include <string.h>

#define HID_TYPE_MAIN 0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL 2
#define HID_TAG_INPUT 8
#define HID_TAG_OUTPUT 9
#define HID_TAG_COLLECTION 10
#define HID_TAG_FEATURE 11
#define HID_GLOBAL_USAGE_PAGE 0
#define HID_GLOBAL_REPORT_SIZE 7
#define HID_GLOBAL_REPORT_ID 8
#define HID_GLOBAL_REPORT_COUNT 9
#define HID_GLOBAL_PUSH 10
#define HID_GLOBAL_POP 11
#define HID_LOCAL_USAGE 0
#define HID_LOCAL_USAGE_MINIMUM 1
#define HID_LOCAL_USAGE_MAXIMUM 2
#define HID_LOCAL_DELIMITER 10
#define HID_COLLECTION_APPLICATION 1
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_KEYBOARD 0x07
#define HID_USAGE_PAGE_CONSUMER 0x0c
#define HID_USAGE_KEYBOARD 0x06
#define HID_USAGE_CONSUMER_CONTROL 0x01
#define HID_GLOBAL_STACK_DEPTH 4
#define HID_LOCAL_USAGE_SET_COUNT 4
#define HID_USAGE_POSITION_LIMIT 256

typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
} global_state_t;

static uint32_t item_value(const uint8_t *data, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint32_t) data[i] << (8 * i);
    return value;
}

static hid_report_descriptor_info_t invalid_descriptor(void) {
    hid_report_descriptor_info_t info = {0};
    return info;
}

static bool scalar_size_valid(size_t size) {
    return size == 1 || size == 2 || size == 4;
}

static uint32_t qualified_usage_page(uint32_t value, size_t size,
                                     uint32_t global_page) {
    return size == 4 ? value >> 16 : global_page;
}

typedef struct {
    uint16_t keyboard_positions[256];
    uint32_t position;
    uint32_t first_consumer_position;
    bool has_consumer_usage;
    bool has_usage;
    bool first_is_keyboard_application;
    bool first_is_consumer_application;
} local_usage_set_t;

static void append_usage(local_usage_set_t *set, uint32_t page,
                         uint32_t usage) {
    if (!set->has_usage) {
        set->has_usage = true;
        set->first_is_keyboard_application =
            page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
            usage == HID_USAGE_KEYBOARD;
        set->first_is_consumer_application =
            page == HID_USAGE_PAGE_CONSUMER &&
            usage == HID_USAGE_CONSUMER_CONTROL;
    }
    if (page == HID_USAGE_PAGE_CONSUMER && !set->has_consumer_usage) {
        set->has_consumer_usage = true;
        set->first_consumer_position = set->position;
    }
    if (page == HID_USAGE_PAGE_KEYBOARD && usage <= 255 &&
        set->position < HID_USAGE_POSITION_LIMIT) {
        uint16_t *first_position = &set->keyboard_positions[usage];
        if (*first_position == UINT16_MAX) {
            *first_position = set->position;
        }
    }
    if (set->position != UINT32_MAX) ++set->position;
}

static uint16_t keyboard_usage_count(const local_usage_set_t *set,
                                     uint32_t report_count) {
    uint16_t count = 0;
    for (size_t i = 0;
         i < sizeof(set->keyboard_positions) /
                 sizeof(set->keyboard_positions[0]);
         ++i) {
        if (set->keyboard_positions[i] < report_count) {
            ++count;
        }
    }
    return count;
}

static void reset_usage_sets(local_usage_set_t sets[HID_LOCAL_USAGE_SET_COUNT]) {
    memset(sets, 0, sizeof(local_usage_set_t) * HID_LOCAL_USAGE_SET_COUNT);
    for (size_t i = 0; i < HID_LOCAL_USAGE_SET_COUNT; ++i) {
        memset(sets[i].keyboard_positions, 0xff,
               sizeof(sets[i].keyboard_positions));
    }
}

hid_report_descriptor_info_t hid_report_descriptor_parse(
    const uint8_t *descriptor, size_t length) {
    hid_report_descriptor_info_t info = {0};
    if (descriptor == NULL || length == 0) return info;

    global_state_t global = {0};
    global_state_t stack[HID_GLOBAL_STACK_DEPTH];
    size_t stack_depth = 0;
    local_usage_set_t usage_sets[HID_LOCAL_USAGE_SET_COUNT];
    reset_usage_sets(usage_sets);
    uint8_t usage_set_count = 1;
    uint8_t current_usage_set = 0;
    bool usage_minimum_pending = false;
    uint32_t usage_minimum_page = 0;
    uint32_t usage_minimum = 0;
    bool delimiter_open = false;
    unsigned collection_depth = 0;
    unsigned keyboard_depth = 0;

    for (size_t offset = 0; offset < length;) {
        uint8_t prefix = descriptor[offset++];
        if (prefix == 0xfe) {
            if (offset + 2 > length) return invalid_descriptor();
            size_t long_size = descriptor[offset];
            if (offset + 2 + long_size > length) return invalid_descriptor();
            offset += 2 + long_size;
            continue;
        }
        size_t size_code = prefix & 3u;
        size_t data_size = size_code == 3 ? 4 : size_code;
        if (offset + data_size > length) return invalid_descriptor();
        uint8_t type = (prefix >> 2) & 3u;
        uint8_t tag = prefix >> 4;
        uint32_t value = item_value(&descriptor[offset], data_size);
        offset += data_size;

        if (type == HID_TYPE_GLOBAL) {
            if (tag == HID_GLOBAL_USAGE_PAGE) {
                if (!scalar_size_valid(data_size)) return invalid_descriptor();
                global.usage_page = value;
            } else if (tag == HID_GLOBAL_REPORT_SIZE) {
                if (!scalar_size_valid(data_size)) return invalid_descriptor();
                global.report_size = value;
            } else if (tag == HID_GLOBAL_REPORT_COUNT) {
                if (!scalar_size_valid(data_size)) return invalid_descriptor();
                global.report_count = value;
            } else if (tag == HID_GLOBAL_REPORT_ID) {
                if (data_size != 1 || value == 0) return invalid_descriptor();
                info.uses_report_ids = true;
            } else if (tag == HID_GLOBAL_PUSH) {
                if (data_size != 0 || stack_depth == HID_GLOBAL_STACK_DEPTH) {
                    return invalid_descriptor();
                }
                stack[stack_depth++] = global;
            } else if (tag == HID_GLOBAL_POP) {
                if (data_size != 0 || stack_depth == 0) {
                    return invalid_descriptor();
                }
                global = stack[--stack_depth];
            }
        } else if (type == HID_TYPE_LOCAL &&
                   (tag == HID_LOCAL_USAGE ||
                    tag == HID_LOCAL_USAGE_MINIMUM ||
                    tag == HID_LOCAL_USAGE_MAXIMUM)) {
            if (!scalar_size_valid(data_size)) return invalid_descriptor();
            uint32_t usage_page = qualified_usage_page(
                value, data_size, global.usage_page);
            uint32_t usage = value & 0xffffu;
            if (tag == HID_LOCAL_USAGE) {
                append_usage(&usage_sets[current_usage_set], usage_page,
                             usage);
            } else if (tag == HID_LOCAL_USAGE_MINIMUM) {
                if (usage_minimum_pending) return invalid_descriptor();
                usage_minimum_pending = true;
                usage_minimum_page = usage_page;
                usage_minimum = usage;
            } else {
                if (!usage_minimum_pending ||
                    usage_page != usage_minimum_page ||
                    usage < usage_minimum) {
                    return invalid_descriptor();
                }
                for (uint32_t item = usage_minimum; item <= usage; ++item) {
                    append_usage(&usage_sets[current_usage_set], usage_page,
                                 item);
                }
                usage_minimum_pending = false;
            }
        } else if (type == HID_TYPE_LOCAL && tag == HID_LOCAL_DELIMITER) {
            if (usage_minimum_pending || data_size != 1 ||
                (value != 0 && value != 1) ||
                (value == 1 && delimiter_open) ||
                (value == 0 && !delimiter_open)) {
                return invalid_descriptor();
            }
            if (value == 1) {
                if (usage_set_count == HID_LOCAL_USAGE_SET_COUNT) {
                    return invalid_descriptor();
                }
                current_usage_set = usage_set_count++;
                delimiter_open = true;
            } else {
                current_usage_set = 0;
                delimiter_open = false;
            }
        } else if (type == HID_TYPE_MAIN) {
            if (delimiter_open || usage_minimum_pending) {
                return invalid_descriptor();
            }
            if ((tag == HID_TAG_INPUT || tag == HID_TAG_OUTPUT ||
                 tag == HID_TAG_FEATURE) && !scalar_size_valid(data_size)) {
                return invalid_descriptor();
            }
            if (tag == HID_TAG_COLLECTION) {
                if (data_size != 1) return invalid_descriptor();
                ++collection_depth;
                if (value == HID_COLLECTION_APPLICATION) {
                    for (uint8_t i = 0; i < usage_set_count; ++i) {
                        if (usage_sets[i].first_is_keyboard_application) {
                            info.has_keyboard = true;
                            keyboard_depth = collection_depth;
                        }
                        if (usage_sets[i].first_is_consumer_application) {
                            info.has_consumer_control = true;
                        }
                    }
                }
            } else if (tag == 12) {
                if (data_size != 0 || collection_depth == 0) {
                    return invalid_descriptor();
                }
                if (keyboard_depth == collection_depth) keyboard_depth = 0;
                --collection_depth;
            } else if (tag == HID_TAG_INPUT && scalar_size_valid(data_size)) {
                for (uint8_t i = 0; i < usage_set_count; ++i) {
                    if (keyboard_depth != 0 && (value & 3u) == 2u &&
                        global.report_size == 1 && global.report_count > 8 &&
                        global.report_count <= HID_USAGE_POSITION_LIMIT &&
                        keyboard_usage_count(&usage_sets[i],
                                             global.report_count) >=
                            global.report_count) {
                        info.has_nkro_keyboard = true;
                    }
                    if ((value & 1u) == 0 &&
                        usage_sets[i].has_consumer_usage &&
                        usage_sets[i].first_consumer_position <
                            global.report_count) {
                        info.has_consumer_control = true;
                    }
                }
            }
            reset_usage_sets(usage_sets);
            usage_set_count = 1;
            current_usage_set = 0;
        }
    }
    if (collection_depth != 0 || stack_depth != 0 || delimiter_open ||
        usage_minimum_pending) {
        return invalid_descriptor();
    }
    info.valid = true;
    return info;
}
