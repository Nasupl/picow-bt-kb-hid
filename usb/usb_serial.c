#include "usb_serial.h"

#include <stdarg.h>
#include <stdio.h>

#include "tusb.h"

#define USB_SERIAL_LOG_BUFFER_SIZE 8192

static uint8_t log_buffer[USB_SERIAL_LOG_BUFFER_SIZE];
static uint16_t log_head;
static uint16_t log_count;
static bool log_overflowed;

bool usb_serial_connected(void) {
    return tud_cdc_connected();
}

void usb_serial_printf(char const *format, ...) {
    if (!tud_cdc_connected()) {
        return;
    }

    char message[128];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length <= 0) {
        return;
    }
    if (length >= (int) sizeof(message)) {
        length = sizeof(message) - 1;
    }

    if ((uint16_t) length > USB_SERIAL_LOG_BUFFER_SIZE - log_count) {
        log_overflowed = true;
        return;
    }
    for (int i = 0; i < length; ++i) {
        uint16_t tail = (uint16_t) ((log_head + log_count) %
                                    USB_SERIAL_LOG_BUFFER_SIZE);
        log_buffer[tail] = (uint8_t) message[i];
        log_count++;
    }
}

void usb_serial_task(void) {
    if (!tud_cdc_connected()) {
        return;
    }

    while (log_count && tud_cdc_write_available()) {
        uint32_t contiguous = USB_SERIAL_LOG_BUFFER_SIZE - log_head;
        if (contiguous > log_count) {
            contiguous = log_count;
        }
        uint32_t available = tud_cdc_write_available();
        if (contiguous > available) {
            contiguous = available;
        }
        uint32_t written = tud_cdc_write(&log_buffer[log_head], contiguous);
        if (written == 0) {
            break;
        }
        log_head = (uint16_t) ((log_head + written) % USB_SERIAL_LOG_BUFFER_SIZE);
        log_count = (uint16_t) (log_count - written);
    }
    tud_cdc_write_flush();

    if (log_overflowed && log_count < USB_SERIAL_LOG_BUFFER_SIZE - 48) {
        log_overflowed = false;
        usb_serial_printf("[USB] CDC log queue overflow; messages dropped\r\n");
    }
}

uint32_t usb_serial_read(void *buffer, uint32_t size) {
    if (!tud_cdc_connected() || size == 0) {
        return 0;
    }
    return tud_cdc_read(buffer, size);
}
