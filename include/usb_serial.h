#ifndef USB_SERIAL_H
#define USB_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

bool usb_serial_connected(void);
void usb_serial_printf(char const *format, ...);
// Drain queued log bytes. Call only from the main TinyUSB task context.
void usb_serial_task(void);
// Read host-to-device CDC bytes from the main task context.
uint32_t usb_serial_read(void *buffer, uint32_t size);

#endif
