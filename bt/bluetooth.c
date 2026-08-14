#include "bluetooth_app.h"
#include "bluetooth_control.h"

#include <stdbool.h>
#include <string.h>

#include "bluetooth_sdp.h"
#include "bt_state.h"
#include "btstack.h"
#include "btstack_hid.h"
#include "btstack_tlv.h"
#include "classic/sdp_client.h"
#include "control_command.h"
#include "control_request_queue.h"
#include "device_manager.h"
#include "hid_boot_report.h"
#include "hid_channels.h"
#include "hid_keyboard.h"
#include "sdp_parser.h"
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#include "usb_serial.h"

static btstack_packet_callback_registration_t hci_event_callback;
static bd_addr_t local_address;
static bd_addr_t connected_device_addr;
static bd_addr_t connecting_device_addr;
static bool connecting_device_valid;
static bool connection_cancel_requested;
static bool stack_working;
static bool startup_logged;
static bt_state_t current_state = STATE_INITIALIZING;
static int inquiry_count = 0;
static btstack_timer_source_t inquiry_timer;
static bool inquiry_timer_armed;
static DeviceManager device_manager;
static void hci_event_handler(uint8_t packet_type,
                              uint16_t channel,
                              uint8_t *packet,
                              uint16_t size);
static void handle_sdp_client_query_result(uint8_t packet_type,
                                           uint16_t channel,
                                           uint8_t *packet,
                                           uint16_t size);
static bool begin_connection(bd_addr_t address);

static uint16_t acl_handle = HCI_CON_HANDLE_INVALID;

static HidChannels hid_channels;
static char command_buffer[32];
static uint8_t command_length;

static SdpParser sdp_parser;
static bd_addr_t selected_device_addr;
static bool selected_device_valid;
static bool auto_connect_enabled = true;
static bool pairing_pin_pending;
static btstack_timer_source_t pairing_pin_timer;
static bool pairing_pin_timer_armed;
static control_request_queue_t control_requests;

#define SELECTED_DEVICE_TAG                                                \
    ((((uint32_t) 'P') << 24) | (((uint32_t) 'K') << 16) |               \
     (((uint32_t) 'B') << 8) | (uint32_t) 'D')

static void load_selected_device(void) {
    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    btstack_tlv_get_instance(&tlv, &context);
    if (tlv == NULL) {
        return;
    }
    int size = tlv->get_tag(context, SELECTED_DEVICE_TAG,
                            selected_device_addr,
                            sizeof(selected_device_addr));
    selected_device_valid = size == (int) sizeof(selected_device_addr);
}

static bool store_selected_device(const bd_addr_t address) {
    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    btstack_tlv_get_instance(&tlv, &context);
    if (tlv == NULL) {
        return false;
    }
    return tlv->store_tag(context, SELECTED_DEVICE_TAG, address,
                          sizeof(bd_addr_t)) == 0;
}

static void delete_selected_device(void) {
    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    btstack_tlv_get_instance(&tlv, &context);
    if (tlv != NULL) {
        tlv->delete_tag(context, SELECTED_DEVICE_TAG);
    }
}

static void clear_pairing_pin(void) {
    pairing_pin_pending = false;
    if (pairing_pin_timer_armed) {
        btstack_run_loop_remove_timer(&pairing_pin_timer);
        pairing_pin_timer_armed = false;
    }
}

static void pairing_pin_timeout(btstack_timer_source_t *timer) {
    (void) timer;
    pairing_pin_timer_armed = false;
    pairing_pin_pending = false;
    usb_serial_printf("[BT] Pairing PIN display expired\r\n");
}

static void show_pairing_pin(void) {
    clear_pairing_pin();
    pairing_pin_pending = true;
    btstack_run_loop_set_timer_handler(&pairing_pin_timer,
                                       pairing_pin_timeout);
    btstack_run_loop_set_timer(&pairing_pin_timer, 30000);
    btstack_run_loop_add_timer(&pairing_pin_timer);
    pairing_pin_timer_armed = true;
}

static void transition_to_state(bt_state_t new_state) {
    if (new_state != STATE_AUTHENTICATING) clear_pairing_pin();
    if (current_state != new_state) {
        if (!bt_state_transition_allowed(current_state, new_state)) {
            usb_serial_printf("[BT] Unexpected state transition %s -> %s\r\n",
                              bt_state_name(current_state),
                              bt_state_name(new_state));
        }
        usb_serial_printf("[BT] State: %s\r\n", bt_state_name(new_state));
        current_state = new_state;
    }
}

static void start_inquiry(void) {
    if (current_state != STATE_IDLE) {
        usb_serial_printf("[BT] Inquiry request ignored in state=%s\r\n",
                          bt_state_name(current_state));
        return;
    }
    if (inquiry_timer_armed) {
        btstack_run_loop_remove_timer(&inquiry_timer);
        inquiry_timer_armed = false;
    }
    transition_to_state(STATE_INQUIRY);
    inquiry_count++;
    usb_serial_printf("[BT] Inquiry #%d started\r\n", inquiry_count);
    gap_inquiry_start(5);
}

static void inquiry_timer_handler(btstack_timer_source_t *ts) {
    (void) ts;
    inquiry_timer_armed = false;
    if (current_state == STATE_IDLE && auto_connect_enabled &&
        selected_device_valid) {
        usb_serial_printf("[BT] Auto-recovery retry\r\n");
        begin_connection(selected_device_addr);
    }
}

static void start_idle_timer(void) {
    if (!auto_connect_enabled || !selected_device_valid) {
        return;
    }
    if (inquiry_timer_armed) {
        usb_serial_printf("[BT] Inquiry retry already scheduled\r\n");
        return;
    }
    usb_serial_printf("[BT] Waiting before next inquiry...\r\n");
    btstack_run_loop_set_timer_handler(&inquiry_timer, inquiry_timer_handler);
    btstack_run_loop_set_timer(&inquiry_timer, 5000);
    btstack_run_loop_add_timer(&inquiry_timer);
    inquiry_timer_armed = true;
}

static bool begin_connection(bd_addr_t address) {
    if (current_state != STATE_IDLE && current_state != STATE_INQUIRY) {
        usb_serial_printf("[BT] Connect request ignored in state=%s\r\n",
                          bt_state_name(current_state));
        return false;
    }
    if (inquiry_timer_armed) {
        btstack_run_loop_remove_timer(&inquiry_timer);
        inquiry_timer_armed = false;
    }
    if (selected_device_valid &&
        memcmp(address, selected_device_addr, sizeof(bd_addr_t)) == 0 &&
        device_manager_find(&device_manager, address) == NULL) {
        bool created;
        Device *saved = device_manager_upsert(
            &device_manager, address, false, 0, 0x0540, NULL, 0, &created);
        if (saved == NULL) {
            usb_serial_printf(
                "[BT] Clearing discovery list to restore saved keyboard\r\n");
            device_manager_init(&device_manager);
            saved = device_manager_upsert(
                &device_manager, address, false, 0, 0x0540, NULL, 0,
                &created);
        }
        if (saved != NULL) {
            link_key_t link_key;
            link_key_type_t link_key_type;
            saved->bonded = gap_get_link_key_for_bd_addr(
                address, link_key, &link_key_type);
        }
    }
    usb_serial_printf("[BT] Connecting to %s...\r\n", bd_addr_to_str(address));
    memcpy(connecting_device_addr, address, sizeof(connecting_device_addr));
    connecting_device_valid = true;
    connection_cancel_requested = false;
    transition_to_state(STATE_CONNECTING);
    uint8_t status = hci_send_cmd(&hci_create_connection, address,
                                  hci_usable_acl_packet_types(), 0, 0, 0, 1);
    if (status != ERROR_CODE_SUCCESS) {
        usb_serial_printf("[BT] Connection failed status=0x%02x\r\n", status);
        connecting_device_valid = false;
        transition_to_state(STATE_IDLE);
        start_idle_timer();
        return false;
    }
    return true;
}

static void cancel_pending_connection(void) {
    if (current_state != STATE_CONNECTING || !connecting_device_valid) return;
    connection_cancel_requested = true;
    uint8_t status = hci_send_cmd(&hci_create_connection_cancel,
                                  connecting_device_addr);
    usb_serial_printf(
        "[BT] Pending connection cancel requested status=0x%02x\r\n",
        status);
}

static void log_device(Device const *device) {
    usb_serial_printf("[BT] Found: %s RSSI=%d CoD=0x%06lx\r\n",
                      bd_addr_to_str(device->address), device->rssi, (unsigned long) device->class_of_device);
}


static Device *get_connected_device(void) {
    for (size_t i = 0; i < device_manager.count; ++i) {
        if (memcmp(device_manager.devices[i].address, connected_device_addr, 6) == 0) {
            return &device_manager.devices[i];
        }
    }
    return NULL;
}

static void reset_hid_channels(void) {
    hid_channels_reset_connection(&hid_channels);
}

static void schedule_keyboard_led_report(void) {
    if (!hid_channels.led_report_pending ||
        hid_channels.led_can_send_requested || !hid_channels.control_open ||
        current_state != STATE_HID_CONNECTED) {
        return;
    }
    uint8_t status = l2cap_request_can_send_now_event(hid_channels.control_cid);
    if (status == ERROR_CODE_SUCCESS) {
        hid_channels.led_can_send_requested = true;
    } else {
        usb_serial_printf(
            "[BT] LED report scheduling failed status=0x%02x\r\n", status);
    }
}

static void release_usb_keyboard(void) {
    keyboard_release_all();
    usb_serial_printf("[USB] RELEASE_ALL\r\n");
}

static void disconnect_acl_after_error(const char *stage, uint8_t status) {
    usb_serial_printf("[BT] %s failed status=0x%02x\r\n", stage, status);
    release_usb_keyboard();
    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(acl_handle);
    } else {
        reset_hid_channels();
        transition_to_state(STATE_IDLE);
        start_idle_timer();
    }
}

static void open_interrupt_channel(void) {
    Device *dev = get_connected_device();
    if (dev == NULL || dev->hid_interrupt_psm == 0) {
        disconnect_acl_after_error("HID Interrupt PSM", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
        return;
    }

    transition_to_state(STATE_OPENING_INTERRUPT);
    usb_serial_printf("[BT] Opening Interrupt Channel (PSM=0x%04x)\r\n",
                      dev->hid_interrupt_psm);
    uint8_t status = l2cap_create_channel(&hci_event_handler,
                                           connected_device_addr,
                                           dev->hid_interrupt_psm,
                                           48,
                                           &hid_channels.interrupt_cid);
    if (status != ERROR_CODE_SUCCESS) {
        disconnect_acl_after_error("Interrupt channel open", status);
    }
}

static void open_control_channel(void) {
    Device *dev = get_connected_device();
    if (dev == NULL || dev->hid_control_psm == 0) {
        disconnect_acl_after_error("HID Control PSM", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
        return;
    }

    transition_to_state(STATE_OPENING_CONTROL);
    usb_serial_printf("[BT] Opening Control Channel (PSM=0x%04x)\r\n",
                      dev->hid_control_psm);
    uint8_t status = l2cap_create_channel(&hci_event_handler,
                                           connected_device_addr,
                                           dev->hid_control_psm,
                                           48,
                                           &hid_channels.control_cid);
    if (status != ERROR_CODE_SUCCESS) {
        disconnect_acl_after_error("Control channel open", status);
    }
}

static void start_sdp_discovery(void) {
    if (current_state != STATE_AUTHENTICATING) {
        return;
    }
    Device *device = get_connected_device();
    if (device != NULL) {
        device->hid_control_psm = 0;
        device->hid_interrupt_psm = 0;
        device->hid_service_found = false;
        device->sdp_completed = false;
    }
    sdp_parser_reset(&sdp_parser);
    transition_to_state(STATE_SERVICE_DISCOVERY);
    usb_serial_printf("[BT] Starting SDP Service Discovery\r\n");
    uint8_t status = sdp_client_query_uuid16(
        &handle_sdp_client_query_result, connected_device_addr,
        BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
    if (status != ERROR_CODE_SUCCESS) {
        disconnect_acl_after_error("SDP start", status);
    }
}

static void handle_hid_data_packet(uint16_t channel, const uint8_t *packet,
                                   uint16_t size) {
    if (channel == hid_channels.control_cid) {
        if (size < 1) {
            return;
        }
        uint8_t message_type = packet[0] >> 4;
        uint8_t status = packet[0] & 0x0f;
        if (current_state == STATE_SETTING_BOOT_PROTOCOL &&
            message_type == HID_MESSAGE_TYPE_HANDSHAKE) {
            usb_serial_printf("[BT] Boot protocol response=0x%02x\r\n", status);
            if (status == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                open_interrupt_channel();
            } else {
                disconnect_acl_after_error("Set Boot Protocol", status);
            }
        }
        return;
    }

    if (channel != hid_channels.interrupt_cid ||
        !hid_channels.interrupt_open) {
        return;
    }
    hid_boot_keyboard_report_t report;
    hid_boot_parse_result_t parse_result = hid_boot_keyboard_parse_input(
        packet, size, &report);
    if (parse_result == HID_BOOT_PARSE_UNSUPPORTED) {
        usb_serial_printf("[HID] Unsupported packet header=0x%02x size=%u\r\n",
                          size ? packet[0] : 0, (unsigned int) size);
        return;
    }
    if (parse_result == HID_BOOT_PARSE_INVALID) {
        usb_serial_printf("[HID] Invalid boot report id=%u size=%u\r\n",
                          (unsigned int) (size == 10 ? packet[1] : 0),
                          (unsigned int) size);
        return;
    }
    usb_serial_printf(
        "[HID] RX id=%u mod=%02x keys=%02x,%02x,%02x,%02x,%02x,%02x\r\n",
        (unsigned int) report.report_id, report.modifier,
        report.keycodes[0], report.keycodes[1], report.keycodes[2],
        report.keycodes[3], report.keycodes[4], report.keycodes[5]);
    keyboard_send_report(report.modifier, report.keycodes);
}

static void handle_sdp_client_query_result(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) packet_type;
    (void) channel;
    (void) size;

    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {

            uint16_t attribute_id     = sdp_event_query_attribute_byte_get_attribute_id(packet);
            uint16_t attribute_length = sdp_event_query_attribute_byte_get_attribute_length(packet);
            uint16_t attribute_offset = sdp_event_query_attribute_byte_get_data_offset(packet);
            uint8_t  attribute_data   = sdp_event_query_attribute_byte_get_data(packet);

            sdp_parser_feed(&sdp_parser, get_connected_device(),
                            attribute_id, attribute_length,
                            attribute_offset, attribute_data);

            break;
        }

        case SDP_EVENT_QUERY_COMPLETE: {
            usb_serial_printf("[BT] Event: SDP_EVENT_QUERY_COMPLETE\r\n");
            uint8_t status = sdp_event_query_complete_get_status(packet);
            usb_serial_printf("[BT] SDP complete\r\n");

            Device *dev = get_connected_device();

            if (status == 0) {
                if (dev) {
                    dev->sdp_completed = true;

                    if (dev->hid_control_psm == 0 ||
                        dev->hid_interrupt_psm == 0) {
                        // Bluetooth HID Classic uses assigned, fixed PSMs.
                        // Some keyboards return an empty SDP result while in
                        // pairing/reconnect mode, so use the standard values.
                        usb_serial_printf(
                            "[BT] SDP attributes=%u; using standard HID PSMs\r\n",
                            (unsigned int)
                                sdp_parser.completed_attribute_count);
                        dev->hid_control_psm = 0x0011;
                        dev->hid_interrupt_psm = 0x0013;
                    }

                    if (dev->hid_control_psm && dev->hid_interrupt_psm) {

                        dev->hid_service_found = true;
                        usb_serial_printf("[BT] HID Service found\r\n");

                        open_control_channel();
                    } else {
                        disconnect_acl_after_error("HID SDP attributes", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                    }
                } else {
                    disconnect_acl_after_error("Connected device lookup", ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
                }
            } else {
                disconnect_acl_after_error("SDP query", status);
            }
            break;
        }

        default:
            usb_serial_printf("[BT] Unknown SDP event 0x%02x\r\n", event);
            break;
    }
}

/* Future placeholders for l2cap/hid segmentation */
static void handle_connected_state(bd_addr_t address, uint16_t handle) {
    (void) address;
    (void) handle;
}

static void handle_disconnected_state(uint16_t handle) {
    (void) handle;
    release_usb_keyboard();
    reset_hid_channels();
    acl_handle = HCI_CON_HANDLE_INVALID;
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    if (packet_type == L2CAP_DATA_PACKET) {
        handle_hid_data_packet(channel, packet, size);
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE:
            usb_serial_printf("[BT] Event: BTSTACK_EVENT_STATE\r\n");
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                gap_local_bd_addr(local_address);
                stack_working = true;
                transition_to_state(STATE_IDLE);

                if (auto_connect_enabled && selected_device_valid) {
                    usb_serial_printf(
                        "[BT] Auto-recovery starting for saved keyboard %s\r\n",
                        bd_addr_to_str(selected_device_addr));
                    begin_connection(selected_device_addr);
                }
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT: {
            usb_serial_printf("[BT] Event: GAP_EVENT_INQUIRY_RESULT\r\n");
            bd_addr_t address;
            gap_event_inquiry_result_get_bd_addr(packet, address);
            bool has_rssi = gap_event_inquiry_result_get_rssi_available(packet);
            int8_t rssi = gap_event_inquiry_result_get_rssi(packet);
            uint32_t class_of_device = gap_event_inquiry_result_get_class_of_device(packet);

            bool created;
            Device *device = device_manager_upsert(
                &device_manager, address,
                has_rssi, rssi, class_of_device,
                gap_event_inquiry_result_get_name_available(packet)
                    ? gap_event_inquiry_result_get_name(packet) : NULL,
                gap_event_inquiry_result_get_name_available(packet)
                    ? gap_event_inquiry_result_get_name_len(packet) : 0,
                &created);
            if (device != NULL && created) {
                log_device(device);
            }
            if (device != NULL && device->hid_supported) {
                link_key_t link_key;
                link_key_type_t link_key_type;
                device->bonded = gap_get_link_key_for_bd_addr(
                    device->address, link_key, &link_key_type);
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (current_state != STATE_INQUIRY) {
                usb_serial_printf(
                    "[BT] Stale inquiry completion ignored in state=%s\r\n",
                    bt_state_name(current_state));
                break;
            }
            usb_serial_printf("[BT] Event: GAP_EVENT_INQUIRY_COMPLETE\r\n");
            usb_serial_printf("[BT] Inquiry #%d complete (%d devices)\r\n",
                              inquiry_count, (int) device_manager.count);

            device_manager_print_devices(&device_manager);

            bd_addr_t target_addr;
            bool have_target = false;
            if (auto_connect_enabled && selected_device_valid) {
                Device *selected = device_manager_find(
                    &device_manager, selected_device_addr);
                if (selected != NULL && selected->hid_supported) {
                    memcpy(target_addr, selected_device_addr, sizeof(target_addr));
                    have_target = true;
                }
            }
            if (auto_connect_enabled && !selected_device_valid && !have_target) {
                have_target = device_manager_get_connection_target(
                    &device_manager, target_addr);
            }
            if (have_target) {
                if (auto_connect_enabled && !selected_device_valid) {
                    memcpy(selected_device_addr, target_addr,
                           sizeof(selected_device_addr));
                    selected_device_valid = true;
                    (void) store_selected_device(selected_device_addr);
                }
                Device *target = device_manager_find(&device_manager, target_addr);
                if (target != NULL && target->bonded) {
                    usb_serial_printf("[BT] Selecting bonded keyboard %s\r\n",
                                      bd_addr_to_str(target_addr));
                }
                begin_connection(target_addr);
            } else {
                transition_to_state(STATE_IDLE);
                start_idle_timer();
            }
            break;

        case HCI_EVENT_CONNECTION_COMPLETE: {
            usb_serial_printf("[BT] Event: HCI_EVENT_CONNECTION_COMPLETE\r\n");
            uint8_t status = hci_event_connection_complete_get_status(packet);
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            bd_addr_t address;
            hci_event_connection_complete_get_bd_addr(packet, address);

            if (current_state != STATE_CONNECTING) {
                usb_serial_printf(
                    "[BT] Stale connection completion ignored status=0x%02x state=%s\r\n",
                    status, bt_state_name(current_state));
                if (status == ERROR_CODE_SUCCESS) {
                    gap_disconnect(handle);
                }
                break;
            }

            connecting_device_valid = false;
            if (connection_cancel_requested) {
                connection_cancel_requested = false;
                usb_serial_printf(
                    "[BT] Explicit disconnect completed pending connection cancellation\r\n");
                if (status == ERROR_CODE_SUCCESS) {
                    acl_handle = handle;
                    transition_to_state(STATE_CONNECTED);
                    gap_disconnect(handle);
                } else {
                    transition_to_state(STATE_IDLE);
                    start_idle_timer();
                }
                break;
            }

            if (status == 0) {
                usb_serial_printf("[BT] Connected handle=0x%04x\r\n", handle);
                acl_handle = handle;
                reset_hid_channels();
                transition_to_state(STATE_CONNECTED);

                Device *dev = device_manager_find(&device_manager, address);
                if (dev) {
                    dev->connected = true;
                }
                memcpy(connected_device_addr, address, 6);

                handle_connected_state(address, handle);

                // Authenticate before SDP. Some legacy keyboards expose no
                // HID attributes while they are in pairing mode until the ACL
                // link has been authenticated and encrypted.
                transition_to_state(STATE_AUTHENTICATING);
                usb_serial_printf("[BT] Requesting encrypted link\r\n");
                gap_request_security_level(acl_handle, LEVEL_2);
            } else {
                usb_serial_printf("[BT] Connection failed status=0x%02x\r\n", status);
                transition_to_state(STATE_IDLE);
                start_idle_timer();
            }
            break;
        }

        case L2CAP_EVENT_CHANNEL_OPENED: {
            usb_serial_printf("[BT] Event: L2CAP_EVENT_CHANNEL_OPENED\r\n");

            uint8_t status = l2cap_event_channel_opened_get_status(packet);

            if (status) {

                usb_serial_printf(
                    "[BT] L2CAP open failed status=0x%02x\r\n",
                    status);

                usb_serial_printf(
                    "[BT] PSM        = 0x%04x\r\n",
                    l2cap_event_channel_opened_get_psm(packet));

                usb_serial_printf(
                    "[BT] Local CID  = 0x%04x\r\n",
                    l2cap_event_channel_opened_get_local_cid(packet));

                usb_serial_printf(
                    "[BT] Remote CID = 0x%04x\r\n",
                    l2cap_event_channel_opened_get_remote_cid(packet));

                disconnect_acl_after_error("L2CAP channel open", status);
                break;
            }

            uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
            uint16_t cid = l2cap_event_channel_opened_get_local_cid(packet);

            usb_serial_printf(
                "[BT] L2CAP opened PSM=0x%04x CID=0x%04x\r\n",
                psm,
                cid);

            Device *dev = get_connected_device();
            if (dev == NULL) {
                disconnect_acl_after_error("L2CAP device lookup", ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
                break;
            }

            if (psm == dev->hid_control_psm) {
                hid_channels_control_opened(&hid_channels, cid);
                transition_to_state(STATE_SETTING_BOOT_PROTOCOL);
                uint8_t request_status = l2cap_request_can_send_now_event(
                    hid_channels.control_cid);
                if (request_status != ERROR_CODE_SUCCESS) {
                    disconnect_acl_after_error("Set Boot Protocol scheduling", request_status);
                }
            } else if (psm == dev->hid_interrupt_psm) {
                hid_channels_interrupt_opened(&hid_channels, cid);

                transition_to_state(STATE_HID_CONNECTED);

                usb_serial_printf("[BT] HID_READY\r\n");
                hid_channels_request_led_sync(&hid_channels);
                schedule_keyboard_led_report();
            }

            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            uint16_t cid = l2cap_event_can_send_now_get_local_cid(packet);
            if (cid == hid_channels.control_cid &&
                hid_channels.boot_protocol_request_pending) {
                hid_channels.boot_protocol_request_pending = false;
                uint8_t request = (uint8_t) ((HID_MESSAGE_TYPE_SET_PROTOCOL << 4) |
                                             HID_PROTOCOL_MODE_BOOT);
                uint8_t status = l2cap_send(
                    hid_channels.control_cid, &request, sizeof(request));
                usb_serial_printf("[BT] Set Boot Protocol sent\r\n");
                if (status != ERROR_CODE_SUCCESS) {
                    disconnect_acl_after_error("Set Boot Protocol send", status);
                }
            } else if (cid == hid_channels.control_cid &&
                       hid_channels.led_can_send_requested) {
                hid_channels.led_can_send_requested = false;
                uint8_t output_packet[2];
                uint8_t output_size = hid_boot_keyboard_output_packet(
                    hid_channels.led_state, output_packet);
                uint8_t status = l2cap_send(
                    hid_channels.control_cid, output_packet, output_size);
                if (status == ERROR_CODE_SUCCESS) {
                    hid_channels.led_report_pending = false;
                    usb_serial_printf("[BT] LED TX bits=0x%02x\r\n",
                                      hid_channels.led_state);
                } else {
                    usb_serial_printf(
                        "[BT] LED report send failed status=0x%02x\r\n",
                        status);
                }
            }
            break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED: {
            uint16_t cid = l2cap_event_channel_closed_get_local_cid(packet);
            usb_serial_printf("[BT] L2CAP channel closed CID=0x%04x\r\n", cid);
            uint8_t closed = hid_channels_closed(&hid_channels, cid);
            bool hid_channel_closed = closed != HID_CHANNEL_CLOSED_NONE;
            if (closed & HID_CHANNEL_CLOSED_INTERRUPT) {
                release_usb_keyboard();
            }
            if (hid_channel_closed && current_state != STATE_DISCONNECTED &&
                acl_handle != HCI_CON_HANDLE_INVALID) {
                gap_disconnect(acl_handle);
            }
            break;
        }

        case GAP_EVENT_SECURITY_LEVEL: {
            uint16_t handle = gap_event_security_level_get_handle(packet);
            uint8_t status = gap_event_security_level_get_status(packet);
            gap_security_level_t level =
                (gap_security_level_t) gap_event_security_level_get_security_level(packet);
            usb_serial_printf("[BT] Security handle=0x%04x level=%u status=0x%02x\r\n",
                              handle, (unsigned int) level, status);
            if (handle != acl_handle || current_state != STATE_AUTHENTICATING) {
                break;
            }
            if (status == ERROR_CODE_SUCCESS && level >= LEVEL_2) {
                start_sdp_discovery();
            } else {
                disconnect_acl_after_error("Authentication",
                                           status ? status : ERROR_CODE_AUTHENTICATION_FAILURE);
            }
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT: {
            uint16_t handle =
                hci_event_authentication_complete_get_connection_handle(packet);
            uint8_t status = hci_event_authentication_complete_get_status(packet);
            usb_serial_printf("[BT] Authentication handle=0x%04x status=0x%02x\r\n",
                              handle, status);
            if (handle == acl_handle && current_state == STATE_AUTHENTICATING &&
                status != ERROR_CODE_SUCCESS) {
                disconnect_acl_after_error("Authentication", status);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE:
        case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
            uint16_t handle = hci_event_encryption_change_get_connection_handle(packet);
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            usb_serial_printf(
                "[BT] Encryption handle=0x%04x enabled=%u status=0x%02x\r\n",
                handle, (unsigned int) enabled, status);
            if (handle != acl_handle || current_state != STATE_AUTHENTICATING) {
                break;
            }
            if (status == ERROR_CODE_SUCCESS && enabled != 0) {
                // Some legacy HID keyboards complete encryption successfully
                // but BTstack does not emit GAP_EVENT_SECURITY_LEVEL promptly.
                start_sdp_discovery();
            } else {
                disconnect_acl_after_error("Encryption",
                                           status ? status : ERROR_CODE_AUTHENTICATION_FAILURE);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            usb_serial_printf("[BT] Event: HCI_EVENT_DISCONNECTION_COMPLETE\r\n");
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);

            usb_serial_printf("[BT] Disconnected reason=0x%02x\r\n", reason);
            usb_serial_printf("[BT] DISCONNECTED\r\n");

            transition_to_state(STATE_DISCONNECTED);
            device_manager_clear_connections(&device_manager);

            handle_disconnected_state(handle);

            transition_to_state(STATE_IDLE);
            start_idle_timer();
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {

            usb_serial_printf("[BT] Event: HCI_EVENT_PIN_CODE_REQUEST\r\n");

            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);

            // A PIN request after a positive Link Key Reply means that the
            // peers no longer share the same key. Remove the stale local key
            // before legacy pairing creates its replacement.
            link_key_t stale_key;
            link_key_type_t stale_key_type;
            bool stale_key_found = gap_get_link_key_for_bd_addr(
                addr, stale_key, &stale_key_type);
            if (stale_key_found) {
                gap_drop_link_key_for_bd_addr(addr);
                Device *device = device_manager_find(&device_manager, addr);
                if (device != NULL) {
                    device->bonded = false;
                }
                usb_serial_printf(
                    "[BT] Stale link key deleted before PIN pairing type=%u\r\n",
                    (unsigned int) stale_key_type);
            }

            usb_serial_printf("[BT] Type PIN 0000 on keyboard, then press Enter\r\n");
            show_pairing_pin();

            hci_send_cmd(&hci_pin_code_request_reply, addr, 4, "0000");

            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            usb_serial_printf("[BT] Event: HCI_EVENT_LINK_KEY_REQUEST\r\n");
            bd_addr_t address;
            link_key_t link_key = {0};
            link_key_type_t link_key_type = INVALID_LINK_KEY;
            hci_event_link_key_request_get_bd_addr(packet, address);
            bool found = gap_get_link_key_for_bd_addr(
                address, link_key, &link_key_type);
            uint8_t response_status = gap_send_link_key_response(
                address, link_key, found ? link_key_type : INVALID_LINK_KEY);
            usb_serial_printf(
                "[BT] Link key response found=%u type=%u status=0x%02x\r\n",
                found ? 1u : 0u,
                found ? (unsigned int) link_key_type :
                        (unsigned int) INVALID_LINK_KEY,
                response_status);
            break;
        }

        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            usb_serial_printf("[BT] Event: HCI_EVENT_LINK_KEY_NOTIFICATION\r\n");
            if (size < 25) {
                usb_serial_printf("[BT] Invalid link key notification size=%u\r\n",
                                  (unsigned int) size);
                break;
            }
            bd_addr_t address;
            // Link Key Request and Notification place BD_ADDR at the same
            // event offset; BTstack itself uses this accessor for both.
            hci_event_link_key_request_get_bd_addr(packet, address);
            link_key_t notified_key;
            memcpy(notified_key, &packet[8], sizeof(notified_key));
            link_key_type_t notified_type = (link_key_type_t) packet[24];

            link_key_t stored_key;
            link_key_type_t stored_type = INVALID_LINK_KEY;
            bool stored = gap_get_link_key_for_bd_addr(
                address, stored_key, &stored_type);
            bool verified = stored && stored_type == notified_type &&
                            memcmp(stored_key, notified_key,
                                   sizeof(stored_key)) == 0;
            bool write_needed = !verified;
            if (write_needed) {
                gap_store_link_key_for_bd_addr(
                    address, notified_key, notified_type);
                stored_type = INVALID_LINK_KEY;
                stored = gap_get_link_key_for_bd_addr(
                    address, stored_key, &stored_type);
                verified = stored && stored_type == notified_type &&
                           memcmp(stored_key, notified_key,
                                  sizeof(stored_key)) == 0;
            }
            usb_serial_printf(
                "[BT] Link key persisted type=%u write_needed=%u verified=%u\r\n",
                (unsigned int) notified_type, write_needed ? 1u : 0u,
                verified ? 1u : 0u);

            Device *device = device_manager_find(&device_manager, address);
            if (device != NULL) {
                device->bonded = verified;
            }
            break;
        }

        case HCI_EVENT_COMMAND_STATUS:
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
        case HCI_EVENT_MAX_SLOTS_CHANGED:
        case HCI_EVENT_PAGE_SCAN_REPETITION_MODE_CHANGE:
        case BTSTACK_EVENT_NR_CONNECTIONS_CHANGED:
            // Routine controller bookkeeping; suppress to keep CDC diagnostics readable.
            break;

        case HCI_EVENT_TRANSPORT_PACKET_SENT:
            // Suppress verbose packet sent log
            break;

        default:
            usb_serial_printf("[BT] Unknown HCI event 0x%02x\r\n", event_type);
            break;
    }
}

static void execute_control_request(
    const bluetooth_control_request_t *request) {
    switch (request->action) {
        case BLUETOOTH_CONTROL_HELP:
            usb_serial_printf("%s\r\n", control_command_help());
            break;
        case BLUETOOTH_CONTROL_STATUS:
            usb_serial_printf(
                "[BT] Status state=%s devices=%u auto_connect=%u selected=%s\r\n",
                bt_state_name(current_state),
                (unsigned int) device_manager.count,
                auto_connect_enabled ? 1u : 0u,
                selected_device_valid
                    ? bd_addr_to_str(selected_device_addr) : "<auto>");
            device_manager_print_devices(&device_manager);
            break;
        case BLUETOOTH_CONTROL_SCAN:
            if (current_state != STATE_IDLE) {
                usb_serial_printf(
                    "[BT] Scan requires Idle state (current=%s)\r\n",
                    bt_state_name(current_state));
            } else {
                auto_connect_enabled = false;
                device_manager_init(&device_manager);
                start_inquiry();
            }
            break;
        case BLUETOOTH_CONTROL_CONNECT: {
            Device *target = device_manager_find(
                &device_manager, request->address);
            if (target == NULL || !target->hid_supported) {
                usb_serial_printf(
                    "[BT] Keyboard %s not found; run scan first\r\n",
                    bd_addr_to_str(request->address));
            } else if (current_state != STATE_IDLE) {
                usb_serial_printf(
                    "[BT] Connect requires Idle state (current=%s)\r\n",
                    bt_state_name(current_state));
            } else {
                memcpy(selected_device_addr, request->address,
                       sizeof(selected_device_addr));
                selected_device_valid = true;
                auto_connect_enabled = true;
                usb_serial_printf(
                    "[BT] Selected keyboard saved=%u\r\n",
                    store_selected_device(selected_device_addr) ? 1u : 0u);
                begin_connection(selected_device_addr);
            }
            break;
        }
        case BLUETOOTH_CONTROL_DISCONNECT:
            auto_connect_enabled = false;
            if (acl_handle != HCI_CON_HANDLE_INVALID) {
                gap_disconnect(acl_handle);
            } else if (current_state == STATE_CONNECTING &&
                       connecting_device_valid) {
                cancel_pending_connection();
            } else {
                usb_serial_printf("[BT] No active keyboard connection\r\n");
            }
            break;
        case BLUETOOTH_CONTROL_RECONNECT: {
            auto_connect_enabled = true;
            if (acl_handle != HCI_CON_HANDLE_INVALID) {
                usb_serial_printf("[BT] Keyboard is already connected\r\n");
                break;
            }
            bd_addr_t target_address;
            bool have_target = false;
            if (selected_device_valid) {
                memcpy(target_address, selected_device_addr,
                       sizeof(target_address));
                have_target = true;
            } else {
                have_target = device_manager_get_connection_target(
                    &device_manager, target_address);
            }
            if (current_state == STATE_IDLE && have_target) {
                if (!selected_device_valid) {
                    memcpy(selected_device_addr, target_address,
                           sizeof(selected_device_addr));
                    selected_device_valid = true;
                    (void) store_selected_device(selected_device_addr);
                }
                begin_connection(target_address);
            } else if (current_state == STATE_IDLE) {
                start_inquiry();
            } else {
                usb_serial_printf(
                    "[BT] Reconnect queued; current state=%s\r\n",
                    bt_state_name(current_state));
            }
            break;
        }
        case BLUETOOTH_CONTROL_FORGET:
            auto_connect_enabled = false;
            selected_device_valid = false;
            delete_selected_device();
            gap_delete_all_link_keys();
            device_manager_clear_bonded(&device_manager);
            usb_serial_printf("[BT] All keyboard bonds deleted\r\n");
            if (acl_handle != HCI_CON_HANDLE_INVALID) {
                gap_disconnect(acl_handle);
            } else {
                cancel_pending_connection();
            }
            break;
    }
}

bool bluetooth_control_submit(const bluetooth_control_request_t *request) {
    return control_request_queue_push(&control_requests, request);
}

void bluetooth_control_get_snapshot(bluetooth_control_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    const char *state = bt_state_name(current_state);
    strncpy(snapshot->state, state, sizeof(snapshot->state) - 1);
    snapshot->auto_connect = auto_connect_enabled;
    snapshot->has_pairing_pin = pairing_pin_pending;
    if (pairing_pin_pending) {
        strncpy(snapshot->pairing_pin, "0000",
                sizeof(snapshot->pairing_pin) - 1);
    }
    snapshot->has_selected_device = selected_device_valid;
    if (selected_device_valid) {
        memcpy(snapshot->selected_device, selected_device_addr,
               sizeof(snapshot->selected_device));
    }
    snapshot->device_count = device_manager.count;
    if (snapshot->device_count > BLUETOOTH_CONTROL_MAX_DEVICES) {
        snapshot->device_count = BLUETOOTH_CONTROL_MAX_DEVICES;
    }
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        const Device *source = &device_manager.devices[i];
        bluetooth_control_device_t *destination = &snapshot->devices[i];
        memcpy(destination->address, source->address,
               sizeof(destination->address));
        if (source->has_name) {
            strncpy(destination->name, source->name,
                    sizeof(destination->name) - 1);
        }
        destination->rssi = source->rssi;
        destination->has_rssi = source->has_rssi;
        destination->keyboard = source->hid_supported;
        destination->bonded = source->bonded;
        destination->connected = source->connected;
    }
}

static bool submit_parsed_command(const control_command_t *command) {
    bluetooth_control_request_t request = {0};
    switch (command->type) {
        case CONTROL_COMMAND_HELP:
            request.action = BLUETOOTH_CONTROL_HELP;
            break;
        case CONTROL_COMMAND_STATUS:
            request.action = BLUETOOTH_CONTROL_STATUS;
            break;
        case CONTROL_COMMAND_SCAN:
            request.action = BLUETOOTH_CONTROL_SCAN;
            break;
        case CONTROL_COMMAND_CONNECT:
            request.action = BLUETOOTH_CONTROL_CONNECT;
            memcpy(request.address, command->address, sizeof(request.address));
            break;
        case CONTROL_COMMAND_DISCONNECT:
            request.action = BLUETOOTH_CONTROL_DISCONNECT;
            break;
        case CONTROL_COMMAND_RECONNECT:
            request.action = BLUETOOTH_CONTROL_RECONNECT;
            break;
        case CONTROL_COMMAND_FORGET:
            request.action = BLUETOOTH_CONTROL_FORGET;
            break;
        default:
            return false;
    }
    return bluetooth_control_submit(&request);
}

void bluetooth_init(void) {
    if (cyw43_arch_init() != PICO_OK) {
        usb_serial_printf("BT: CYW43 initialization failed\r\n");
        return;
    }

    hid_channels_init(&hid_channels);
    control_request_queue_init(&control_requests);
    load_selected_device();
    l2cap_init();
    sdp_client_init();
    device_manager_init(&device_manager);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    hci_event_callback.callback = hci_event_handler;
    hci_add_event_handler(&hci_event_callback);
    hci_power_control(HCI_POWER_ON);
}

void bluetooth_task(void) {
    async_context_poll(cyw43_arch_async_context());
    schedule_keyboard_led_report();

    uint8_t input[16];
    uint32_t count = usb_serial_read(input, sizeof(input));
    for (uint32_t i = 0; i < count; ++i) {
        char ch = (char) input[i];
        if (ch == '\r' || ch == '\n') {
            if (command_length == 0) {
                continue;
            }
            command_buffer[command_length] = '\0';
            control_command_t command;
            if (!control_command_parse(command_buffer, &command)) {
                usb_serial_printf("[BT] Unknown command: %s\r\n%s\r\n",
                                  command_buffer, control_command_help());
                command_length = 0;
                continue;
            }
            if (!submit_parsed_command(&command)) {
                usb_serial_printf("[BT] Control request queue full\r\n");
            }
            command_length = 0;
        } else if (command_length < sizeof(command_buffer) - 1) {
            command_buffer[command_length++] = ch;
        } else {
            command_length = 0;
            usb_serial_printf("[BT] Command too long\r\n");
        }
    }

    bluetooth_control_request_t request;
    while (control_request_queue_pop(&control_requests, &request)) {
        execute_control_request(&request);
    }

    if (stack_working && !startup_logged && usb_serial_connected()) {
        startup_logged = true;
        usb_serial_printf("[BT] Stack started; adapter address %s\r\n",
                          bd_addr_to_str(local_address));

    }
}

void bluetooth_set_keyboard_leds(uint8_t leds) {
    (void) hid_channels_set_leds(&hid_channels, leds);
}

void bluetooth_forward_key_event(bool pressed, uint8_t modifier, uint8_t keycode) {
    if (pressed) {
        keyboard_press(modifier, keycode);
    } else {
        keyboard_release();
    }
}
