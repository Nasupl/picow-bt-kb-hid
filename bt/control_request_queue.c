#include "control_request_queue.h"

#include <string.h>

void control_request_queue_init(control_request_queue_t *queue) {
    memset(queue, 0, sizeof(*queue));
}

bool control_request_queue_push(control_request_queue_t *queue,
                                const bluetooth_control_request_t *request) {
    if (queue == NULL || request == NULL ||
        queue->count == CONTROL_REQUEST_QUEUE_CAPACITY) {
        return false;
    }
    size_t tail = (queue->head + queue->count) % CONTROL_REQUEST_QUEUE_CAPACITY;
    queue->requests[tail] = *request;
    queue->count++;
    return true;
}

bool control_request_queue_pop(control_request_queue_t *queue,
                               bluetooth_control_request_t *request) {
    if (queue == NULL || request == NULL || queue->count == 0) {
        return false;
    }
    *request = queue->requests[queue->head];
    queue->head = (queue->head + 1) % CONTROL_REQUEST_QUEUE_CAPACITY;
    queue->count--;
    return true;
}
