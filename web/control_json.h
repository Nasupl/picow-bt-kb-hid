#ifndef CONTROL_JSON_H
#define CONTROL_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "bluetooth_control.h"

// Covers the worst case where every byte in every device name needs a
// six-byte JSON escape, plus all fixed fields and terminators.
#define CONTROL_JSON_MAX_SNAPSHOT_SIZE 14336

bool control_json_write_snapshot(const bluetooth_control_snapshot_t *snapshot,
                                 char *buffer, size_t capacity,
                                 size_t *written);

#endif
