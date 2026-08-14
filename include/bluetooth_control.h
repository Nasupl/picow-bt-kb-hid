#ifndef BLUETOOTH_CONTROL_H
#define BLUETOOTH_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLUETOOTH_CONTROL_MAX_DEVICES 16
#define BLUETOOTH_CONTROL_NAME_SIZE 64
#define BLUETOOTH_CONTROL_STATE_SIZE 24
#define BLUETOOTH_CONTROL_PIN_SIZE 7
#define BLUETOOTH_CONTROL_VERSION_SIZE 32

typedef enum {
    BLUETOOTH_CONTROL_HELP = 0,
    BLUETOOTH_CONTROL_STATUS,
    BLUETOOTH_CONTROL_SCAN,
    BLUETOOTH_CONTROL_CONNECT,
    BLUETOOTH_CONTROL_DISCONNECT,
    BLUETOOTH_CONTROL_RECONNECT,
    BLUETOOTH_CONTROL_FORGET,
} bluetooth_control_action_t;

typedef struct {
    bluetooth_control_action_t action;
    uint8_t address[6];
} bluetooth_control_request_t;

typedef struct {
    uint8_t address[6];
    char name[BLUETOOTH_CONTROL_NAME_SIZE];
    int8_t rssi;
    bool has_rssi;
    bool keyboard;
    bool bonded;
    bool connected;
    bool report_descriptor_present;
    bool report_descriptor_valid;
    bool report_has_keyboard;
    bool report_has_consumer_control;
    bool report_has_nkro_keyboard;
    bool report_uses_ids;
} bluetooth_control_device_t;

typedef struct {
    char version[BLUETOOTH_CONTROL_VERSION_SIZE];
    char state[BLUETOOTH_CONTROL_STATE_SIZE];
    bool auto_connect;
    bool has_pairing_pin;
    char pairing_pin[BLUETOOTH_CONTROL_PIN_SIZE];
    bool has_selected_device;
    uint8_t selected_device[6];
    size_t device_count;
    bluetooth_control_device_t devices[BLUETOOTH_CONTROL_MAX_DEVICES];
} bluetooth_control_snapshot_t;

// Queues an operation for execution from bluetooth_task(). Callers must run in
// the same cooperative CYW43 poll context as bluetooth_task(); the operation
// does not invoke BTstack directly from the UI callback.
bool bluetooth_control_submit(const bluetooth_control_request_t *request);

// Copies the latest connection state for CDC, HTTP, or other presentation
// layers. The returned data owns no pointers into Bluetooth internals.
void bluetooth_control_get_snapshot(bluetooth_control_snapshot_t *snapshot);

#endif
