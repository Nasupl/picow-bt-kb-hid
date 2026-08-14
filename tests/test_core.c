#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_manager.h"
#include "bt_state.h"
#include "control_command.h"
#include "control_request_queue.h"
#include "control_json.h"
#include "hid_boot_report.h"
#include "hid_channels.h"
#include "sdp_parser.h"

const char *bd_addr_to_str(const uint8_t address[6]) {
    (void) address;
    return "test-address";
}

void usb_serial_printf(const char *format, ...) {
    (void) format;
}

static void test_boot_reports(void) {
    const uint8_t released[8] = {0};
    const uint8_t key_a[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0};
    const uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    const uint8_t reserved_byte[8] = {0, 0xff, 0x04, 0, 0, 0, 0, 0};
    const uint8_t mixed_error[8] = {0, 0, 1, 0x04, 1, 1, 1, 1};

    assert(hid_boot_keyboard_report_valid(released));
    assert(hid_boot_keyboard_report_valid(key_a));
    assert(hid_boot_keyboard_report_valid(rollover));
    assert(!hid_boot_keyboard_report_valid(reserved_byte));
    assert(!hid_boot_keyboard_report_valid(mixed_error));

    uint8_t output[2] = {0};
    assert(hid_boot_keyboard_output_packet(0x07, output) == 2);
    assert(output[0] == 0xa2 && output[1] == 0x07);
    assert(hid_boot_keyboard_output_packet(0xff, output) == 2);
    assert(output[1] == 0x1f);

    const uint8_t with_id[10] = {
        0xa1, 1, 0x02, 0, 0x04, 0x05, 0, 0, 0, 0,
    };
    hid_boot_keyboard_report_t parsed;
    assert(hid_boot_keyboard_parse_input(
               with_id, sizeof(with_id), &parsed) == HID_BOOT_PARSE_OK);
    assert(parsed.report_id == 1 && parsed.modifier == 0x02);
    assert(parsed.keycodes[0] == 0x04 && parsed.keycodes[1] == 0x05);

    const uint8_t without_id[9] = {
        0xa1, 0x01, 0, 0x06, 0, 0, 0, 0, 0,
    };
    assert(hid_boot_keyboard_parse_input(
               without_id, sizeof(without_id), &parsed) == HID_BOOT_PARSE_OK);
    assert(parsed.report_id == 0 && parsed.modifier == 0x01);
    assert(parsed.keycodes[0] == 0x06);

    uint8_t wrong_header[9] = {0xa2};
    assert(hid_boot_keyboard_parse_input(
               wrong_header, sizeof(wrong_header), &parsed) ==
           HID_BOOT_PARSE_UNSUPPORTED);
    assert(hid_boot_keyboard_parse_input(
               without_id, 8, &parsed) == HID_BOOT_PARSE_UNSUPPORTED);

    uint8_t malformed[9] = {0xa1, 0, 1, 0x04, 1, 1, 1, 1, 0};
    assert(hid_boot_keyboard_parse_input(
               malformed, sizeof(malformed), &parsed) ==
           HID_BOOT_PARSE_INVALID);
    assert(hid_boot_keyboard_parse_input(
               NULL, sizeof(without_id), &parsed) ==
           HID_BOOT_PARSE_UNSUPPORTED);
}

static void test_device_manager(void) {
    DeviceManager manager;
    device_manager_init(&manager);
    assert(manager.count == 0);

    const uint8_t keyboard[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t phone[6] = {6, 5, 4, 3, 2, 1};
    bool created = false;
    Device *device = device_manager_upsert(
        &manager, keyboard, true, -42, 0x002540,
        (const uint8_t *) "Keyboard", 8, &created);
    assert(device != NULL && created && device->hid_supported);
    assert(device->has_name && strcmp(device->name, "Keyboard") == 0);

    device = device_manager_upsert(
        &manager, keyboard, true, -30, 0x002540, NULL, 0, &created);
    assert(device != NULL && !created && manager.count == 1);
    assert(device->rssi == -30 && strcmp(device->name, "Keyboard") == 0);

    device = device_manager_upsert(
        &manager, phone, false, 0, 0x5a020c, NULL, 0, &created);
    assert(device != NULL && created && !device->hid_supported);

    uint8_t target[6] = {0};
    assert(device_manager_get_connection_target(&manager, target));
    assert(memcmp(target, keyboard, sizeof(target)) == 0);

    const uint8_t bonded_keyboard[6] = {7, 7, 7, 7, 7, 7};
    device = device_manager_upsert(
        &manager, bonded_keyboard, true, -80, 0x002540, NULL, 0, &created);
    assert(device != NULL && created);
    device->bonded = true;
    assert(device_manager_get_connection_target(&manager, target));
    assert(memcmp(target, bonded_keyboard, sizeof(target)) == 0);
    device_manager_clear_bonded(&manager);
    assert(!device->bonded);

    manager.devices[0].connected = true;
    device_manager_clear_connections(&manager);
    assert(!manager.devices[0].connected);
}

static void test_device_limit_and_name_truncation(void) {
    DeviceManager manager;
    device_manager_init(&manager);
    uint8_t long_name[80];
    memset(long_name, 'x', sizeof(long_name));

    for (uint8_t i = 0; i < DEVICE_MANAGER_MAX_DEVICES; ++i) {
        uint8_t address[6] = {0, 0, 0, 0, 0, i};
        bool created = false;
        Device *device = device_manager_upsert(
            &manager, address, false, 0, 0, long_name, sizeof(long_name),
            &created);
        assert(device != NULL && created);
        assert(strlen(device->name) == DEVICE_MANAGER_NAME_MAX_LENGTH - 1);
    }
    uint8_t extra[6] = {9, 9, 9, 9, 9, 9};
    bool created = false;
    assert(device_manager_upsert(
               &manager, extra, false, 0, 0, NULL, 0, &created) == NULL);
    assert(!created);
}

static void test_bluetooth_state_machine(void) {
    const bt_state_t happy_path[] = {
        STATE_IDLE,
        STATE_INQUIRY,
        STATE_CONNECTING,
        STATE_CONNECTED,
        STATE_AUTHENTICATING,
        STATE_SERVICE_DISCOVERY,
        STATE_OPENING_CONTROL,
        STATE_SETTING_BOOT_PROTOCOL,
        STATE_OPENING_INTERRUPT,
        STATE_HID_CONNECTED,
    };
    for (size_t i = 1; i < sizeof(happy_path) / sizeof(happy_path[0]); ++i) {
        assert(bt_state_transition_allowed(happy_path[i - 1], happy_path[i]));
    }
    assert(bt_state_transition_allowed(STATE_IDLE, STATE_CONNECTING));
    assert(bt_state_transition_allowed(STATE_CONNECTING, STATE_IDLE));
    assert(bt_state_transition_allowed(STATE_HID_CONNECTED, STATE_DISCONNECTED));
    assert(!bt_state_transition_allowed(STATE_IDLE, STATE_HID_CONNECTED));
    assert(!bt_state_transition_allowed(STATE_INQUIRY, STATE_OPENING_CONTROL));
    assert(strcmp(bt_state_name(STATE_HID_CONNECTED), "HIDConnected") == 0);
    assert(strcmp(bt_state_name((bt_state_t) 100), "Unknown") == 0);
}

static void test_hid_channel_state(void) {
    HidChannels channels;
    hid_channels_init(&channels);
    assert(!channels.control_open && !channels.interrupt_open);

    hid_channels_control_opened(&channels, 0x40);
    assert(channels.control_open && channels.control_cid == 0x40);
    assert(channels.boot_protocol_request_pending);
    hid_channels_interrupt_opened(&channels, 0x41);
    assert(channels.interrupt_open && channels.interrupt_cid == 0x41);

    assert(hid_channels_set_leds(&channels, 0xff));
    assert(channels.led_state == 0x1f && channels.led_report_pending);
    channels.led_report_pending = false;
    assert(!hid_channels_set_leds(&channels, 0x1f));
    hid_channels_request_led_sync(&channels);
    assert(channels.led_report_pending);

    assert(hid_channels_closed(&channels, 0x99) == HID_CHANNEL_CLOSED_NONE);
    assert(hid_channels_closed(&channels, 0x41) ==
           HID_CHANNEL_CLOSED_INTERRUPT);
    assert(!channels.interrupt_open && channels.control_open);

    channels.led_can_send_requested = true;
    hid_channels_reset_connection(&channels);
    assert(!channels.control_open && channels.control_cid == 0);
    assert(!channels.led_can_send_requested);
    assert(channels.led_state == 0x1f);
    assert(channels.led_report_pending);
}

static void feed_sdp(SdpParser *parser, Device *device, uint16_t attribute,
                     const uint8_t *data, uint16_t size) {
    for (uint16_t i = 0; i < size; ++i) {
        sdp_parser_feed(parser, device, attribute, size, i, data[i]);
    }
}

static void test_sdp_parser(void) {
    // ProtocolDescriptorList: L2CAP PSM 0x0011, HIDP.
    const uint8_t control[] = {
        0x35, 0x0d,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x11,
        0x35, 0x03, 0x19, 0x00, 0x11,
    };
    // AdditionalProtocolDescriptorLists with nested Interrupt descriptor.
    const uint8_t interrupt[] = {
        0x35, 0x0f, 0x35, 0x0d,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x13,
        0x35, 0x03, 0x19, 0x00, 0x11,
    };
    const uint8_t service_name[] = {
        0x25, 0x08, 'K', 'e', 'y', 'b', 'o', 'a', 'r', 'd',
    };

    SdpParser parser = {0};
    Device device = {0};
    feed_sdp(&parser, &device, 0x0004, control, sizeof(control));
    feed_sdp(&parser, &device, 0x000d, interrupt, sizeof(interrupt));
    feed_sdp(&parser, &device, 0x0100, service_name, sizeof(service_name));
    assert(device.hid_control_psm == 0x0011);
    assert(device.hid_interrupt_psm == 0x0013);
    assert(device.has_name && strcmp(device.name, "Keyboard") == 0);
    assert(parser.completed_attribute_count == 3);

    sdp_parser_reset(&parser);
    assert(parser.completed_attribute_count == 0);
    uint8_t malformed[] = {0x35, 0xff};
    feed_sdp(&parser, &device, 0x0004, malformed, sizeof(malformed));
    assert(parser.completed_attribute_count == 1);
}

static void test_control_commands(void) {
    control_command_t command;

    assert(control_command_parse("help", &command));
    assert(command.type == CONTROL_COMMAND_HELP);
    assert(control_command_parse("  status  ", &command));
    assert(command.type == CONTROL_COMMAND_STATUS);
    assert(control_command_parse("scan", &command));
    assert(command.type == CONTROL_COMMAND_SCAN);
    assert(control_command_parse("disconnect", &command));
    assert(command.type == CONTROL_COMMAND_DISCONNECT);
    assert(control_command_parse("reconnect", &command));
    assert(command.type == CONTROL_COMMAND_RECONNECT);
    assert(control_command_parse("forget", &command));
    assert(command.type == CONTROL_COMMAND_FORGET);

    assert(control_command_parse("connect 01:23:45:67:89:aB", &command));
    assert(command.type == CONTROL_COMMAND_CONNECT);
    const uint8_t expected[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
    assert(memcmp(command.address, expected, sizeof(expected)) == 0);

    assert(!control_command_parse("connect 01:23:45:67:89", &command));
    assert(command.type == CONTROL_COMMAND_INVALID);
    assert(!control_command_parse("connect nope", &command));
    assert(!control_command_parse("unknown", &command));
    assert(strstr(control_command_help(), "connect") != NULL);
}

static void test_control_request_queue(void) {
    control_request_queue_t queue;
    control_request_queue_init(&queue);
    bluetooth_control_request_t request = {
        .action = BLUETOOTH_CONTROL_SCAN,
    };
    bluetooth_control_request_t output;

    assert(!control_request_queue_pop(&queue, &output));
    for (size_t i = 0; i < CONTROL_REQUEST_QUEUE_CAPACITY; ++i) {
        request.action = (bluetooth_control_action_t) i;
        request.address[0] = (uint8_t) i;
        assert(control_request_queue_push(&queue, &request));
    }
    assert(!control_request_queue_push(&queue, &request));
    for (size_t i = 0; i < CONTROL_REQUEST_QUEUE_CAPACITY; ++i) {
        assert(control_request_queue_pop(&queue, &output));
        assert(output.action == (bluetooth_control_action_t) i);
        assert(output.address[0] == i);
    }

    // Exercise wrap-around after the head has advanced.
    request.action = BLUETOOTH_CONTROL_RECONNECT;
    assert(control_request_queue_push(&queue, &request));
    assert(control_request_queue_pop(&queue, &output));
    assert(output.action == BLUETOOTH_CONTROL_RECONNECT);
}

static void test_control_snapshot_json(void) {
    bluetooth_control_snapshot_t snapshot = {0};
    strcpy(snapshot.version, "v1.2.3");
    strcpy(snapshot.state, "HIDConnected");
    snapshot.auto_connect = true;
    snapshot.has_pairing_pin = true;
    strcpy(snapshot.pairing_pin, "0000");
    snapshot.has_selected_device = true;
    const uint8_t address[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
    memcpy(snapshot.selected_device, address, sizeof(address));
    snapshot.device_count = 1;
    memcpy(snapshot.devices[0].address, address, sizeof(address));
    strcpy(snapshot.devices[0].name, "Key\"board\\test\n");
    snapshot.devices[0].rssi = -42;
    snapshot.devices[0].has_rssi = true;
    snapshot.devices[0].keyboard = true;
    snapshot.devices[0].bonded = true;
    snapshot.devices[0].connected = true;

    char json[768];
    size_t written = 0;
    assert(control_json_write_snapshot(&snapshot, json, sizeof(json), &written));
    assert(written == strlen(json));
    assert(strstr(json, "\"state\":\"HIDConnected\"") != NULL);
    assert(strstr(json, "\"version\":\"v1.2.3\"") != NULL);
    assert(strstr(json, "\"pairingPin\":\"0000\"") != NULL);
    assert(strstr(json, "\"selectedDevice\":\"01:23:45:67:89:AB\"") != NULL);
    assert(strstr(json, "Key\\\"board\\\\test\\u000a") != NULL);
    assert(strstr(json, "\"rssi\":-42") != NULL);
    assert(strstr(json, "\"connected\":true") != NULL);

    char too_small[16];
    assert(!control_json_write_snapshot(&snapshot, too_small,
                                        sizeof(too_small), NULL));
    assert(too_small[sizeof(too_small) - 1] == '\0');
    assert(!control_json_write_snapshot(NULL, json, sizeof(json), NULL));

    snapshot.has_pairing_pin = false;
    assert(control_json_write_snapshot(&snapshot, json, sizeof(json), NULL));
    assert(strstr(json, "\"pairingPin\":null") != NULL);

    bluetooth_control_snapshot_t worst_case = {0};
    memset(worst_case.state, 1, sizeof(worst_case.state) - 1);
    worst_case.device_count = BLUETOOTH_CONTROL_MAX_DEVICES;
    for (size_t i = 0; i < worst_case.device_count; ++i) {
        memset(worst_case.devices[i].name, 1,
               sizeof(worst_case.devices[i].name) - 1);
    }
    char maximum_json[CONTROL_JSON_MAX_SNAPSHOT_SIZE];
    assert(control_json_write_snapshot(&worst_case, maximum_json,
                                       sizeof(maximum_json), &written));
}

int main(void) {
    test_boot_reports();
    test_device_manager();
    test_device_limit_and_name_truncation();
    test_bluetooth_state_machine();
    test_hid_channel_state();
    test_sdp_parser();
    test_control_commands();
    test_control_request_queue();
    test_control_snapshot_json();
    puts("core host tests: PASS");
    return 0;
}
