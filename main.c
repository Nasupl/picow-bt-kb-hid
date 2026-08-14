#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "bluetooth_app.h"
#include "hid_keyboard.h"
#include "tusb.h"

int main(void) {
    board_init();

    const tusb_rhport_init_t rhport_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUD_OPT_HIGH_SPEED ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,
    };
    if (!tud_rhport_init(BOARD_TUD_RHPORT, &rhport_init)) {
        return 1;
    }
    board_init_after_tusb();
    bluetooth_init();

    while (true) {
        hid_keyboard_task();
        bluetooth_task();
    }
}
