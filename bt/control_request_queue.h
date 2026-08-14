#ifndef CONTROL_REQUEST_QUEUE_H
#define CONTROL_REQUEST_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "bluetooth_control.h"

#define CONTROL_REQUEST_QUEUE_CAPACITY 8

typedef struct {
    bluetooth_control_request_t requests[CONTROL_REQUEST_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} control_request_queue_t;

void control_request_queue_init(control_request_queue_t *queue);
bool control_request_queue_push(control_request_queue_t *queue,
                                const bluetooth_control_request_t *request);
bool control_request_queue_pop(control_request_queue_t *queue,
                               bluetooth_control_request_t *request);

#endif
