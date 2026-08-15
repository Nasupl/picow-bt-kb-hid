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

static void mark_keyboard_usage(uint8_t usages[32], uint32_t usage,
                                uint16_t *count) {
    if (usage > 255) return;
    uint8_t mask = (uint8_t) (1u << (usage & 7u));
    uint8_t *entry = &usages[usage >> 3];
    if ((*entry & mask) == 0) {
        *entry |= mask;
        ++*count;
    }
}

hid_report_descriptor_info_t hid_report_descriptor_parse(
    const uint8_t *descriptor, size_t length) {
    hid_report_descriptor_info_t info = {0};
    if (descriptor == NULL || length == 0) return info;

    global_state_t global = {0};
    global_state_t stack[HID_GLOBAL_STACK_DEPTH];
    size_t stack_depth = 0;
    uint8_t local_keyboard_usages[32] = {0};
    uint16_t local_keyboard_usage_count = 0;
    bool local_consumer_usage = false;
    bool local_keyboard_application = false;
    bool local_consumer_application = false;
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
                if (usage_page == HID_USAGE_PAGE_KEYBOARD) {
                    mark_keyboard_usage(local_keyboard_usages, usage,
                                        &local_keyboard_usage_count);
                }
                local_consumer_usage |= usage_page == HID_USAGE_PAGE_CONSUMER;
                local_keyboard_application |=
                    usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                    usage == HID_USAGE_KEYBOARD;
                local_consumer_application |=
                    usage_page == HID_USAGE_PAGE_CONSUMER &&
                    usage == HID_USAGE_CONSUMER_CONTROL;
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
                if (usage_page == HID_USAGE_PAGE_KEYBOARD) {
                    uint32_t last = usage < 255 ? usage : 255;
                    if (usage_minimum <= last) {
                        for (uint32_t item = usage_minimum; item <= last;
                             ++item) {
                            mark_keyboard_usage(local_keyboard_usages, item,
                                                &local_keyboard_usage_count);
                        }
                    }
                }
                local_consumer_usage |= usage_page == HID_USAGE_PAGE_CONSUMER;
                local_keyboard_application |=
                    usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                    usage_minimum <= HID_USAGE_KEYBOARD &&
                    usage >= HID_USAGE_KEYBOARD;
                local_consumer_application |=
                    usage_page == HID_USAGE_PAGE_CONSUMER &&
                    usage_minimum <= HID_USAGE_CONSUMER_CONTROL &&
                    usage >= HID_USAGE_CONSUMER_CONTROL;
                usage_minimum_pending = false;
            }
        } else if (type == HID_TYPE_LOCAL && tag == HID_LOCAL_DELIMITER) {
            if (data_size != 1 || (value != 0 && value != 1) ||
                (value == 1 && delimiter_open) ||
                (value == 0 && !delimiter_open)) {
                return invalid_descriptor();
            }
            delimiter_open = value == 1;
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
                    if (local_keyboard_application) {
                        info.has_keyboard = true;
                        keyboard_depth = collection_depth;
                    }
                    if (local_consumer_application) {
                        info.has_consumer_control = true;
                    }
                }
            } else if (tag == 12) {
                if (data_size != 0 || collection_depth == 0) {
                    return invalid_descriptor();
                }
                if (keyboard_depth == collection_depth) keyboard_depth = 0;
                --collection_depth;
            } else if (tag == HID_TAG_INPUT && keyboard_depth != 0 &&
                       scalar_size_valid(data_size) && (value & 3u) == 2u &&
                       global.report_size == 1 && global.report_count > 8 &&
                       local_keyboard_usage_count >= global.report_count) {
                info.has_nkro_keyboard = true;
            }
            if (tag == HID_TAG_INPUT && scalar_size_valid(data_size) &&
                (value & 1u) == 0 && local_consumer_usage) {
                info.has_consumer_control = true;
            }
            memset(local_keyboard_usages, 0, sizeof(local_keyboard_usages));
            local_keyboard_usage_count = 0;
            local_consumer_usage = false;
            local_keyboard_application = false;
            local_consumer_application = false;
        }
    }
    if (collection_depth != 0 || stack_depth != 0 || delimiter_open) {
        return invalid_descriptor();
    }
    info.valid = true;
    return info;
}
