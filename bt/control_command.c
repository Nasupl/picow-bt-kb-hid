#include "control_command.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (char) tolower((unsigned char) ch);
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool parse_address(const char *text, uint8_t address[6]) {
    for (size_t i = 0; i < 6; ++i) {
        int high = hex_value(text[0]);
        int low = hex_value(text[1]);
        if (high < 0 || low < 0) {
            return false;
        }
        address[i] = (uint8_t) ((high << 4) | low);
        text += 2;
        if (i != 5) {
            if (*text != ':') {
                return false;
            }
            text++;
        }
    }
    return *text == '\0';
}

bool control_command_parse(const char *line, control_command_t *command) {
    if (line == NULL || command == NULL) {
        return false;
    }
    memset(command, 0, sizeof(*command));

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t length = strlen(line);
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t')) {
        length--;
    }

    if (length == 4 && strncmp(line, "help", length) == 0) {
        command->type = CONTROL_COMMAND_HELP;
    } else if (length == 6 && strncmp(line, "status", length) == 0) {
        command->type = CONTROL_COMMAND_STATUS;
    } else if (length == 4 && strncmp(line, "scan", length) == 0) {
        command->type = CONTROL_COMMAND_SCAN;
    } else if (length == 10 && strncmp(line, "disconnect", length) == 0) {
        command->type = CONTROL_COMMAND_DISCONNECT;
    } else if (length == 9 && strncmp(line, "reconnect", length) == 0) {
        command->type = CONTROL_COMMAND_RECONNECT;
    } else if (length == 6 && strncmp(line, "forget", length) == 0) {
        command->type = CONTROL_COMMAND_FORGET;
    } else if (length == 25 && strncmp(line, "connect ", 8) == 0 &&
               parse_address(line + 8, command->address)) {
        command->type = CONTROL_COMMAND_CONNECT;
    } else {
        command->type = CONTROL_COMMAND_INVALID;
        return false;
    }
    return true;
}

const char *control_command_help(void) {
    return "Commands: help, status, scan, connect XX:XX:XX:XX:XX:XX, "
           "disconnect, reconnect, forget";
}
