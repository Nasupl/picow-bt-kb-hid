#include "control_json.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool valid;
} json_writer_t;

static void append_format(json_writer_t *writer, const char *format, ...) {
    if (!writer->valid) {
        return;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(writer->buffer + writer->length,
                           writer->capacity - writer->length, format, args);
    va_end(args);
    if (length < 0 || (size_t) length >= writer->capacity - writer->length) {
        writer->valid = false;
        return;
    }
    writer->length += (size_t) length;
}

static void append_json_string(json_writer_t *writer, const char *value) {
    append_format(writer, "\"");
    for (const unsigned char *cursor = (const unsigned char *) value;
         writer->valid && *cursor; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            append_format(writer, "\\%c", *cursor);
        } else if (*cursor < 0x20) {
            append_format(writer, "\\u%04x", (unsigned int) *cursor);
        } else {
            append_format(writer, "%c", *cursor);
        }
    }
    append_format(writer, "\"");
}

static void append_address(json_writer_t *writer, const uint8_t address[6]) {
    append_format(writer, "\"%02X:%02X:%02X:%02X:%02X:%02X\"",
                  address[0], address[1], address[2], address[3], address[4],
                  address[5]);
}

bool control_json_write_snapshot(const bluetooth_control_snapshot_t *snapshot,
                                 char *buffer, size_t capacity,
                                 size_t *written) {
    if (snapshot == NULL || buffer == NULL || capacity == 0) {
        return false;
    }
    json_writer_t writer = {
        .buffer = buffer,
        .capacity = capacity,
        .valid = true,
    };
    buffer[0] = '\0';

    append_format(&writer, "{\"version\":");
    append_json_string(&writer, snapshot->version);
    append_format(&writer, ",\"state\":");
    append_json_string(&writer, snapshot->state);
    append_format(&writer, ",\"autoConnect\":%s,\"pairingPin\":",
                  snapshot->auto_connect ? "true" : "false");
    if (snapshot->has_pairing_pin) {
        append_json_string(&writer, snapshot->pairing_pin);
    } else {
        append_format(&writer, "null");
    }
    append_format(&writer, ",\"selectedDevice\":");
    if (snapshot->has_selected_device) {
        append_address(&writer, snapshot->selected_device);
    } else {
        append_format(&writer, "null");
    }
    append_format(&writer, ",\"devices\":[");
    size_t device_count = snapshot->device_count;
    if (device_count > BLUETOOTH_CONTROL_MAX_DEVICES) {
        device_count = BLUETOOTH_CONTROL_MAX_DEVICES;
    }
    for (size_t i = 0; i < device_count; ++i) {
        const bluetooth_control_device_t *device = &snapshot->devices[i];
        append_format(&writer, "%s{\"address\":", i ? "," : "");
        append_address(&writer, device->address);
        append_format(&writer, ",\"name\":");
        append_json_string(&writer, device->name);
        append_format(
            &writer,
            ",\"rssi\":%d,\"hasRssi\":%s,\"keyboard\":%s,"
            "\"bonded\":%s,\"connected\":%s}",
            device->rssi, device->has_rssi ? "true" : "false",
            device->keyboard ? "true" : "false",
            device->bonded ? "true" : "false",
            device->connected ? "true" : "false");
    }
    append_format(&writer, "]}");

    if (!writer.valid) {
        buffer[capacity - 1] = '\0';
        return false;
    }
    if (written != NULL) {
        *written = writer.length;
    }
    return true;
}
