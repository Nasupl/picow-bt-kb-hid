#include "hid_report_descriptor.h"

#define HID_TYPE_MAIN 0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL 2
#define HID_TAG_INPUT 8
#define HID_TAG_COLLECTION 10
#define HID_GLOBAL_USAGE_PAGE 0
#define HID_GLOBAL_REPORT_SIZE 7
#define HID_GLOBAL_REPORT_ID 8
#define HID_GLOBAL_REPORT_COUNT 9
#define HID_GLOBAL_PUSH 10
#define HID_GLOBAL_POP 11
#define HID_LOCAL_USAGE 0
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

hid_report_descriptor_info_t hid_report_descriptor_parse(
    const uint8_t *descriptor, size_t length) {
    hid_report_descriptor_info_t info = {0};
    if (descriptor == NULL || length == 0) return info;

    global_state_t global = {0};
    global_state_t stack[HID_GLOBAL_STACK_DEPTH];
    size_t stack_depth = 0;
    uint32_t local_usage = 0;
    bool have_local_usage = false;
    unsigned collection_depth = 0;
    unsigned keyboard_depth = 0;
    unsigned consumer_depth = 0;

    for (size_t offset = 0; offset < length;) {
        uint8_t prefix = descriptor[offset++];
        if (prefix == 0xfe) {
            if (offset + 2 > length) return info;
            size_t long_size = descriptor[offset];
            if (offset + 2 + long_size > length) return info;
            offset += 2 + long_size;
            continue;
        }
        size_t size_code = prefix & 3u;
        size_t data_size = size_code == 3 ? 4 : size_code;
        if (offset + data_size > length) return info;
        uint8_t type = (prefix >> 2) & 3u;
        uint8_t tag = prefix >> 4;
        uint32_t value = item_value(&descriptor[offset], data_size);
        offset += data_size;

        if (type == HID_TYPE_GLOBAL) {
            if (tag == HID_GLOBAL_USAGE_PAGE) global.usage_page = value;
            else if (tag == HID_GLOBAL_REPORT_SIZE) global.report_size = value;
            else if (tag == HID_GLOBAL_REPORT_COUNT) global.report_count = value;
            else if (tag == HID_GLOBAL_REPORT_ID) {
                if (value == 0) return info;
                info.uses_report_ids = true;
            } else if (tag == HID_GLOBAL_PUSH) {
                if (stack_depth == HID_GLOBAL_STACK_DEPTH) return info;
                stack[stack_depth++] = global;
            } else if (tag == HID_GLOBAL_POP) {
                if (stack_depth == 0) return info;
                global = stack[--stack_depth];
            }
        } else if (type == HID_TYPE_LOCAL && tag == HID_LOCAL_USAGE) {
            local_usage = value;
            have_local_usage = true;
        } else if (type == HID_TYPE_MAIN) {
            if (tag == HID_TAG_COLLECTION) {
                ++collection_depth;
                if (value == HID_COLLECTION_APPLICATION && have_local_usage) {
                    if (global.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                        local_usage == HID_USAGE_KEYBOARD) {
                        info.has_keyboard = true;
                        keyboard_depth = collection_depth;
                    } else if (global.usage_page == HID_USAGE_PAGE_CONSUMER &&
                               local_usage == HID_USAGE_CONSUMER_CONTROL) {
                        info.has_consumer_control = true;
                        consumer_depth = collection_depth;
                    }
                }
            } else if (tag == 12) {
                if (collection_depth == 0) return info;
                if (keyboard_depth == collection_depth) keyboard_depth = 0;
                if (consumer_depth == collection_depth) consumer_depth = 0;
                --collection_depth;
            } else if (tag == HID_TAG_INPUT && keyboard_depth != 0 &&
                       global.usage_page == HID_USAGE_PAGE_KEYBOARD &&
                       global.report_size == 1 && global.report_count > 8) {
                info.has_nkro_keyboard = true;
            }
            have_local_usage = false;
        }
    }
    info.valid = collection_depth == 0 && stack_depth == 0 &&
                 (info.has_keyboard || info.has_consumer_control);
    return info;
}
