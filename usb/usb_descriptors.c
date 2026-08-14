#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

enum {
    ITF_NUM_HID,
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL,
};

enum {
    STRID_LANGID,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

static tusb_desc_device_t const device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,
    .idProduct = 0x4005,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *) &device_descriptor;
}

static uint8_t const hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void) instance;
    return hid_report_descriptor;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
#define EPNUM_HID 0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT 0x03
#define EPNUM_CDC_IN 0x83

static uint8_t const configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return configuration_descriptor;
}

static char const *const string_descriptors[] = {
    (const char[]) {0x09, 0x04},
    "Pico BT Keyboard",
    "Pico W USB HID Keyboard",
    NULL,
};

static uint16_t string_descriptor[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    size_t count;

    if (index == STRID_LANGID) {
        memcpy(&string_descriptor[1], string_descriptors[0], 2);
        count = 1;
    } else if (index == STRID_SERIAL) {
        count = board_usb_get_serial(&string_descriptor[1], 32);
    } else {
        if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
            return NULL;
        }
        char const *const text = string_descriptors[index];
        count = strlen(text);
        if (count > 32) {
            count = 32;
        }
        for (size_t i = 0; i < count; ++i) {
            string_descriptor[1 + i] = text[i];
        }
    }

    string_descriptor[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return string_descriptor;
}
