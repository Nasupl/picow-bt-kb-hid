cmake_minimum_required(VERSION 3.13)

# Select Pico W before importing the SDK so CYW43 support is enabled.
set(PICO_BOARD pico_w CACHE STRING "Target board")

if (NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to the Pico SDK")
endif()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to the Pico SDK directory.")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_bt_keyboard C CXX ASM)

pico_sdk_init()

option(PICO_KEYBOARD_DEMO "Type a fixed USB keyboard demo after enumeration" OFF)

add_executable(pico_bt_keyboard
    main.c
    bt/bluetooth.c
    bt/bt_state.c
    bt/control_command.c
    bt/control_request_queue.c
    bt/device_manager.c
    bt/hid_boot_report.c
    bt/hid_channels.c
    bt/sdp_parser.c
    usb/hid_keyboard.c
    usb/usb_serial.c
    usb/usb_descriptors.c
)

if (PICO_KEYBOARD_DEMO)
    target_sources(pico_bt_keyboard PRIVATE usb/keyboard_demo.c)
    target_compile_definitions(pico_bt_keyboard PRIVATE ENABLE_KEYBOARD_DEMO=1)
endif()

target_include_directories(pico_bt_keyboard PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/bt
)

target_compile_options(pico_bt_keyboard PRIVATE -Wall -Wextra)

target_link_libraries(pico_bt_keyboard
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_classic
)

# This firmware owns the USB device stack for HID; do not add the SDK CDC
# stdio interface, which would change the USB device composition.
pico_enable_stdio_usb(pico_bt_keyboard 0)
pico_enable_stdio_uart(pico_bt_keyboard 0)

pico_add_extra_outputs(pico_bt_keyboard)
