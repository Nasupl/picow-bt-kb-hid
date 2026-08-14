#ifndef HID_CHANNELS_H
#define HID_CHANNELS_H

#include <stdbool.h>
#include <stdint.h>

enum {
    HID_CHANNEL_CLOSED_NONE = 0,
    HID_CHANNEL_CLOSED_CONTROL = 1,
    HID_CHANNEL_CLOSED_INTERRUPT = 2,
};

typedef struct {
    uint16_t control_cid;
    uint16_t interrupt_cid;
    bool control_open;
    bool interrupt_open;
    bool boot_protocol_request_pending;
    uint8_t led_state;
    bool led_report_pending;
    bool led_can_send_requested;
} HidChannels;

void hid_channels_init(HidChannels *channels);
void hid_channels_reset_connection(HidChannels *channels);
void hid_channels_control_opened(HidChannels *channels, uint16_t cid);
void hid_channels_interrupt_opened(HidChannels *channels, uint16_t cid);
uint8_t hid_channels_closed(HidChannels *channels, uint16_t cid);
bool hid_channels_set_leds(HidChannels *channels, uint8_t leds);
void hid_channels_request_led_sync(HidChannels *channels);

#endif
