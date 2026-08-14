#ifndef CONTROL_COMMAND_H
#define CONTROL_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONTROL_COMMAND_NONE = 0,
    CONTROL_COMMAND_HELP,
    CONTROL_COMMAND_STATUS,
    CONTROL_COMMAND_SCAN,
    CONTROL_COMMAND_CONNECT,
    CONTROL_COMMAND_DISCONNECT,
    CONTROL_COMMAND_RECONNECT,
    CONTROL_COMMAND_FORGET,
    CONTROL_COMMAND_INVALID,
} control_command_type_t;

typedef struct {
    control_command_type_t type;
    uint8_t address[6];
} control_command_t;

bool control_command_parse(const char *line, control_command_t *command);
const char *control_command_help(void);

#endif
