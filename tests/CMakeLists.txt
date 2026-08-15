cmake_minimum_required(VERSION 3.13)
project(pico_bt_keyboard_host_tests C)

enable_testing()

add_executable(core_tests
    test_core.c
    ../bt/control_command.c
    ../bt/control_request_queue.c
    ../bt/device_manager.c
    ../bt/bt_state.c
    ../bt/hid_channels.c
    ../bt/hid_report_descriptor.c
    ../bt/hid_report_map.c
    ../bt/sdp_parser.c
    ../bt/hid_boot_report.c
    ../web/control_json.c
)
target_include_directories(core_tests PRIVATE
    ../bt
    ../include
    ../web
    stubs
)
target_compile_options(core_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME core_tests COMMAND core_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME smoke_summary_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_smoke_test.py
)
