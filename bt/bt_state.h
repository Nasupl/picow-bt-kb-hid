#ifndef BT_STATE_H
#define BT_STATE_H

#include <stdbool.h>

typedef enum {
    STATE_INITIALIZING,
    STATE_IDLE,
    STATE_INQUIRY,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_SERVICE_DISCOVERY,
    STATE_AUTHENTICATING,
    STATE_READY,
    STATE_DISCONNECTED,
    STATE_OPENING_CONTROL,
    STATE_SETTING_BOOT_PROTOCOL,
    STATE_OPENING_INTERRUPT,
    STATE_HID_CONNECTED,
} bt_state_t;

const char *bt_state_name(bt_state_t state);
bool bt_state_transition_allowed(bt_state_t from, bt_state_t to);

#endif
