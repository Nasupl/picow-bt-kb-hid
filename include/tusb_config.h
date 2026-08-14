#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT           0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED        OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the Pico SDK.
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                OPT_OS_NONE
#endif
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           BOARD_TUD_MAX_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_HID                 1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_HID_EP_BUFSIZE      16
#define CFG_TUD_CDC_RX_BUFSIZE      64
#define CFG_TUD_CDC_TX_BUFSIZE      128

#ifdef __cplusplus
}
#endif

#endif
