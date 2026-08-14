#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEVICE_MANAGER_MAX_DEVICES 16
#define DEVICE_MANAGER_NAME_MAX_LENGTH 64

typedef struct {
    uint8_t address[6];
    char name[DEVICE_MANAGER_NAME_MAX_LENGTH];
    int8_t rssi;
    uint32_t class_of_device;
    bool connected;
    bool bonded;
    bool hid_supported;
    bool has_rssi;
    bool has_name;
    bool hid_service_found;
    uint16_t hid_control_psm;
    uint16_t hid_interrupt_psm;
    bool sdp_completed;
} Device;

typedef struct {
    Device devices[DEVICE_MANAGER_MAX_DEVICES];
    size_t count;
} DeviceManager;

void device_manager_init(DeviceManager *manager);
Device *device_manager_upsert(DeviceManager *manager, const uint8_t address[6],
                              bool has_rssi, int8_t rssi, uint32_t class_of_device,
                              const uint8_t *name, size_t name_length,
                              bool *created);
bool device_manager_get_connection_target(const DeviceManager *manager, uint8_t address[6]);
Device *device_manager_find(DeviceManager *manager, const uint8_t address[6]);
void device_manager_print_devices(const DeviceManager *manager);
void device_manager_clear_connections(DeviceManager *manager);
void device_manager_clear_bonded(DeviceManager *manager);

#endif
