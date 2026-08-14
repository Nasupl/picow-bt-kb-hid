#ifndef CONTROL_JSON_H
#define CONTROL_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "bluetooth_control.h"

bool control_json_write_snapshot(const bluetooth_control_snapshot_t *snapshot,
                                 char *buffer, size_t capacity,
                                 size_t *written);

#endif
