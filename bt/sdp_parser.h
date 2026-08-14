#ifndef SDP_PARSER_H
#define SDP_PARSER_H

#include <stdint.h>

#include "device_manager.h"

#define SDP_PARSER_BUFFER_SIZE 256

typedef struct {
    uint8_t buffer[SDP_PARSER_BUFFER_SIZE];
    uint16_t completed_attribute_count;
} SdpParser;

void sdp_parser_reset(SdpParser *parser);
void sdp_parser_feed(SdpParser *parser, Device *device,
                     uint16_t attribute_id, uint16_t attribute_length,
                     uint16_t attribute_offset, uint8_t data);

#endif
