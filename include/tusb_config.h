#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "pico/stdio_usb.h"

#if !defined(LIB_TINYUSB_HOST) && !defined(LIB_TINYUSB_DEVICE)
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE  64
#endif

#define CFG_TUD_CDC             1
#define CFG_TUD_NCM             1
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_EP_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif

#define CFG_TUD_NCM_IN_NTB_MAX_SIZE   2048
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE  2048
#define CFG_TUD_NCM_OUT_NTB_N         1
#define CFG_TUD_NCM_IN_NTB_N          2

#endif
