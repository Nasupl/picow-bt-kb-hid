#include "sdp_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hid_report_descriptor.h"
#include "usb_serial.h"

#define SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST 0x0004
#define SDP_ATTR_ADDITIONAL_PROTOCOL_DESCRIPTOR_LISTS 0x000d
#define SDP_ATTR_SERVICE_NAME 0x0100
#define SDP_ATTR_PROVIDER_NAME 0x0102
#define SDP_ATTR_HID_DESCRIPTOR_LIST 0x0206

#define DE_TYPE_UINT 1
#define DE_TYPE_UUID 3
#define DE_TYPE_STRING 4
#define DE_TYPE_SEQUENCE 6

typedef struct {
    uint8_t type;
    const uint8_t *data;
    uint16_t data_size;
    uint16_t total_size;
} DataElement;

static bool read_element(const uint8_t *buffer, uint16_t available,
                         DataElement *element) {
    if (buffer == NULL || element == NULL || available == 0) return false;

    uint8_t size_index = buffer[0] & 7;
    uint16_t header_size = 1;
    uint32_t data_size;
    if (size_index <= 4) {
        data_size = 1u << size_index;
    } else {
        uint8_t length_bytes = (uint8_t) (1u << (size_index - 5));
        if ((uint16_t) (1 + length_bytes) > available) return false;
        header_size = (uint16_t) (1 + length_bytes);
        data_size = 0;
        for (uint8_t i = 0; i < length_bytes; ++i) {
            data_size = (data_size << 8) | buffer[1 + i];
        }
    }
    if (data_size > UINT16_MAX ||
        header_size + data_size > available) return false;

    element->type = buffer[0] >> 3;
    element->data = &buffer[header_size];
    element->data_size = (uint16_t) data_size;
    element->total_size = (uint16_t) (header_size + data_size);
    return true;
}

static bool element_uint16(const DataElement *element, uint16_t *value) {
    if (element->data_size != 2 ||
        (element->type != DE_TYPE_UINT && element->type != DE_TYPE_UUID)) {
        return false;
    }
    *value = (uint16_t) ((element->data[0] << 8) | element->data[1]);
    return true;
}

static bool find_report_descriptor(const uint8_t *buffer, uint16_t size,
                                   unsigned depth, const uint8_t **descriptor,
                                   uint16_t *descriptor_size) {
    if (depth > 4) return false;
    DataElement root;
    if (!read_element(buffer, size, &root) || root.type != DE_TYPE_SEQUENCE) {
        return false;
    }
    uint16_t offset = 0;
    DataElement first = {0};
    if (read_element(root.data, root.data_size, &first) &&
        first.type == DE_TYPE_UINT && first.data_size == 1 &&
        first.data[0] == 0x22) {
        offset = first.total_size;
        DataElement report;
        if (offset < root.data_size &&
            read_element(&root.data[offset],
                         (uint16_t) (root.data_size - offset), &report) &&
            report.type == DE_TYPE_STRING) {
            *descriptor = report.data;
            *descriptor_size = report.data_size;
            return true;
        }
    }
    offset = 0;
    while (offset < root.data_size) {
        DataElement child;
        if (!read_element(&root.data[offset],
                          (uint16_t) (root.data_size - offset), &child)) {
            return false;
        }
        if (child.type == DE_TYPE_SEQUENCE &&
            find_report_descriptor(&root.data[offset], child.total_size,
                                   depth + 1, descriptor, descriptor_size)) {
            return true;
        }
        offset = (uint16_t) (offset + child.total_size);
    }
    return false;
}

static bool find_l2cap_psm(const uint8_t *buffer, uint16_t size,
                           uint16_t *psm) {
    DataElement root;
    if (!read_element(buffer, size, &root) ||
        root.type != DE_TYPE_SEQUENCE) return false;

    uint16_t offset = 0;
    while (offset < root.data_size) {
        DataElement child;
        if (!read_element(&root.data[offset],
                          (uint16_t) (root.data_size - offset), &child)) {
            return false;
        }
        if (child.type == DE_TYPE_SEQUENCE) {
            DataElement first;
            if (read_element(child.data, child.data_size, &first)) {
                uint16_t uuid;
                if (first.type == DE_TYPE_UUID &&
                    element_uint16(&first, &uuid) && uuid == 0x0100) {
                    DataElement second;
                    uint16_t next = first.total_size;
                    if (next < child.data_size &&
                        read_element(&child.data[next],
                                     (uint16_t) (child.data_size - next),
                                     &second) &&
                        second.type == DE_TYPE_UINT &&
                        element_uint16(&second, psm)) {
                        return true;
                    }
                }
            }
            if (find_l2cap_psm(&root.data[offset], child.total_size, psm)) {
                return true;
            }
        }
        offset = (uint16_t) (offset + child.total_size);
    }
    return false;
}

static void parse_attribute(Device *device, uint16_t attribute_id,
                            const uint8_t *buffer, uint16_t length) {
    if (attribute_id == SDP_ATTR_HID_DESCRIPTOR_LIST) {
        if (device == NULL) return;
        device->report_descriptor_present = true;
        const uint8_t *descriptor;
        uint16_t descriptor_size;
        if (!find_report_descriptor(buffer, length, 0, &descriptor,
                                    &descriptor_size)) {
            usb_serial_printf("[BT] HID Report Descriptor malformed\r\n");
            return;
        }
        hid_report_descriptor_info_t info = hid_report_descriptor_parse(
            descriptor, descriptor_size);
        device->report_descriptor_valid = info.valid;
        device->report_has_keyboard = info.has_keyboard;
        device->report_has_consumer_control = info.has_consumer_control;
        device->report_has_nkro_keyboard = info.has_nkro_keyboard;
        device->report_uses_ids = info.uses_report_ids;
        usb_serial_printf(
            "[BT] HID Report Descriptor valid=%u keyboard=%u consumer=%u "
            "nkro=%u report_ids=%u bytes=%u\r\n",
            info.valid, info.has_keyboard, info.has_consumer_control,
            info.has_nkro_keyboard, info.uses_report_ids,
            (unsigned int) descriptor_size);
        return;
    }
    if (attribute_id == SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST ||
        attribute_id == SDP_ATTR_ADDITIONAL_PROTOCOL_DESCRIPTOR_LISTS) {
        uint16_t psm;
        if (!find_l2cap_psm(buffer, length, &psm)) return;
        bool control = attribute_id == SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST;
        usb_serial_printf("[BT] %s PSM: 0x%04x\r\n",
                          control ? "Control" : "Interrupt", psm);
        if (device != NULL) {
            if (control) device->hid_control_psm = psm;
            else device->hid_interrupt_psm = psm;
        }
        return;
    }

    if (attribute_id != SDP_ATTR_SERVICE_NAME &&
        attribute_id != SDP_ATTR_PROVIDER_NAME) return;
    DataElement text_element;
    if (!read_element(buffer, length, &text_element) ||
        text_element.type != DE_TYPE_STRING) return;
    uint16_t text_length = text_element.data_size;
    if (text_length >= DEVICE_MANAGER_NAME_MAX_LENGTH) {
        text_length = DEVICE_MANAGER_NAME_MAX_LENGTH - 1;
    }
    char text[DEVICE_MANAGER_NAME_MAX_LENGTH];
    memcpy(text, text_element.data, text_length);
    text[text_length] = '\0';
    bool service = attribute_id == SDP_ATTR_SERVICE_NAME;
    usb_serial_printf("[BT] %s Name: %s\r\n",
                      service ? "Service" : "Provider", text);
    if (service && device != NULL && !device->has_name && text_length > 0) {
        memcpy(device->name, text, text_length + 1);
        device->has_name = true;
    }
}

void sdp_parser_reset(SdpParser *parser) {
    if (parser != NULL) parser->completed_attribute_count = 0;
}

void sdp_parser_feed(SdpParser *parser, Device *device,
                     uint16_t attribute_id, uint16_t attribute_length,
                     uint16_t attribute_offset, uint8_t data) {
    if (parser == NULL || attribute_length == 0 ||
        attribute_offset >= attribute_length) return;

    if (attribute_length > sizeof(parser->buffer)) {
        if (attribute_id == SDP_ATTR_HID_DESCRIPTOR_LIST && device != NULL) {
            device->report_descriptor_present = true;
            device->report_descriptor_valid = false;
        }
        return;
    }

    parser->buffer[attribute_offset] = data;
    if (attribute_offset + 1 == attribute_length) {
        parser->completed_attribute_count++;
        parse_attribute(device, attribute_id, parser->buffer,
                        attribute_length);
    }
}
