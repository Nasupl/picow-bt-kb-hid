#include "bt_state.h"

const char *bt_state_name(bt_state_t state) {
    switch (state) {
        case STATE_INITIALIZING: return "Initializing";
        case STATE_IDLE: return "Idle";
        case STATE_INQUIRY: return "Inquiry";
        case STATE_CONNECTING: return "Connecting";
        case STATE_CONNECTED: return "Connected";
        case STATE_SERVICE_DISCOVERY: return "ServiceDiscovery";
        case STATE_AUTHENTICATING: return "Authenticating";
        case STATE_READY: return "Ready";
        case STATE_DISCONNECTED: return "Disconnected";
        case STATE_OPENING_CONTROL: return "OpeningControl";
        case STATE_SETTING_BOOT_PROTOCOL: return "SettingBootProtocol";
        case STATE_OPENING_INTERRUPT: return "OpeningInterrupt";
        case STATE_HID_CONNECTED: return "HIDConnected";
        default: return "Unknown";
    }
}

bool bt_state_transition_allowed(bt_state_t from, bt_state_t to) {
    if (from == to) {
        return true;
    }
    // Error recovery can return to Idle, while a controller disconnect can
    // arrive from any state after ACL creation.
    if (to == STATE_IDLE || to == STATE_DISCONNECTED) {
        return true;
    }
    switch (from) {
        case STATE_IDLE: return to == STATE_INQUIRY;
        case STATE_INQUIRY: return to == STATE_CONNECTING;
        case STATE_CONNECTING: return to == STATE_CONNECTED;
        case STATE_CONNECTED: return to == STATE_AUTHENTICATING;
        case STATE_AUTHENTICATING: return to == STATE_SERVICE_DISCOVERY;
        case STATE_SERVICE_DISCOVERY: return to == STATE_OPENING_CONTROL;
        case STATE_OPENING_CONTROL: return to == STATE_SETTING_BOOT_PROTOCOL;
        case STATE_SETTING_BOOT_PROTOCOL: return to == STATE_OPENING_INTERRUPT;
        case STATE_OPENING_INTERRUPT: return to == STATE_HID_CONNECTED;
        case STATE_DISCONNECTED: return to == STATE_IDLE;
        case STATE_INITIALIZING:
        case STATE_READY:
        case STATE_HID_CONNECTED:
        default: return false;
    }
}
