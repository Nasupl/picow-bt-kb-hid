#include "hid_channels.h"

#include <string.h>

void hid_channels_init(HidChannels *channels) {
    memset(channels, 0, sizeof(*channels));
}

void hid_channels_reset_connection(HidChannels *channels) {
    channels->control_cid = 0;
    channels->interrupt_cid = 0;
    channels->control_open = false;
    channels->interrupt_open = false;
    channels->boot_protocol_request_pending = false;
    channels->led_can_send_requested = false;
}

void hid_channels_control_opened(HidChannels *channels, uint16_t cid) {
    channels->control_cid = cid;
    channels->control_open = true;
    channels->boot_protocol_request_pending = true;
}

void hid_channels_interrupt_opened(HidChannels *channels, uint16_t cid) {
    channels->interrupt_cid = cid;
    channels->interrupt_open = true;
}

uint8_t hid_channels_closed(HidChannels *channels, uint16_t cid) {
    uint8_t result = HID_CHANNEL_CLOSED_NONE;
    if (cid != 0 && cid == channels->control_cid) {
        channels->control_cid = 0;
        channels->control_open = false;
        channels->boot_protocol_request_pending = false;
        channels->led_can_send_requested = false;
        result |= HID_CHANNEL_CLOSED_CONTROL;
    }
    if (cid != 0 && cid == channels->interrupt_cid) {
        channels->interrupt_cid = 0;
        channels->interrupt_open = false;
        result |= HID_CHANNEL_CLOSED_INTERRUPT;
    }
    return result;
}

bool hid_channels_set_leds(HidChannels *channels, uint8_t leds) {
    leds &= 0x1f;
    if (channels->led_state == leds && !channels->led_report_pending) {
        return false;
    }
    channels->led_state = leds;
    channels->led_report_pending = true;
    return true;
}

void hid_channels_request_led_sync(HidChannels *channels) {
    channels->led_report_pending = true;
}
