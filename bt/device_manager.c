#include "device_manager.h"

#include <string.h>

void device_manager_init(DeviceManager *manager) {
    memset(manager, 0, sizeof(*manager));
}

Device *device_manager_find(DeviceManager *manager, const uint8_t address[6]) {
    for (size_t i = 0; i < manager->count; ++i) {
        if (memcmp(manager->devices[i].address, address, sizeof(manager->devices[i].address)) == 0) {
            return &manager->devices[i];
        }
    }
    return NULL;
}

Device *device_manager_upsert(DeviceManager *manager, const uint8_t address[6],
                              bool has_rssi, int8_t rssi, uint32_t class_of_device,
                              const uint8_t *name, size_t name_length,
                              bool *created) {
    Device *device = device_manager_find(manager, address);
    *created = false;

    if (device == NULL) {
        if (manager->count == DEVICE_MANAGER_MAX_DEVICES) {
            return NULL;
        }
        device = &manager->devices[manager->count++];
        memset(device, 0, sizeof(*device));
        memcpy(device->address, address, sizeof(device->address));
        *created = true;
    }

    device->class_of_device = class_of_device;
    // Check if the device is a Keyboard (Peripheral major device class, Keyboard minor class)
    if (((class_of_device & 0x1f00) == 0x0500) && (class_of_device & 0x40)) {
        device->hid_supported = true;
    } else {
        device->hid_supported = false;
    }

    device->has_rssi = has_rssi;
    if (has_rssi) {
        device->rssi = rssi;
    }
    if (name != NULL && name_length != 0) {
        if (name_length >= sizeof(device->name)) {
            name_length = sizeof(device->name) - 1;
        }
        memcpy(device->name, name, name_length);
        device->name[name_length] = '\0';
        device->has_name = true;
    }
    return device;
}

#include "btstack.h"
#include "usb_serial.h"

bool device_manager_get_connection_target(const DeviceManager *manager, uint8_t address[6]) {
    for (size_t i = 0; i < manager->count; ++i) {
        if (manager->devices[i].hid_supported && manager->devices[i].bonded) {
            memcpy(address, manager->devices[i].address, 6);
            return true;
        }
    }
    for (size_t i = 0; i < manager->count; ++i) {
        if (manager->devices[i].hid_supported) {
            memcpy(address, manager->devices[i].address, 6);
            return true;
        }
    }
    return false;
}

void device_manager_print_devices(const DeviceManager *manager) {
    usb_serial_printf("[BT] Discovered Devices (%u):\r\n", (unsigned int) manager->count);
    for (size_t i = 0; i < manager->count; ++i) {
        const Device *device = &manager->devices[i];
        usb_serial_printf("[BT] - %s: RSSI=%d, CoD=0x%06lx, Name=%s, HID=%s, Bonded=%s, Connected=%s, SDP=%s, CtrlPSM=0x%04x, IntPSM=0x%04x\r\n",
                          bd_addr_to_str(device->address),
                          device->rssi,
                          (unsigned long) device->class_of_device,
                          device->has_name ? device->name : "<unknown>",
                          device->hid_supported ? "Yes" : "No",
                          device->bonded ? "Yes" : "No",
                          device->connected ? "Yes" : "No",
                          device->sdp_completed ? "Done" : "No",
                          device->hid_control_psm,
                          device->hid_interrupt_psm);
    }
}

void device_manager_clear_connections(DeviceManager *manager) {
    for (size_t i = 0; i < manager->count; ++i) {
        manager->devices[i].connected = false;
    }
}

void device_manager_clear_bonded(DeviceManager *manager) {
    for (size_t i = 0; i < manager->count; ++i) {
        manager->devices[i].bonded = false;
    }
}
