#include <stdio.h>
#include "tusb.h"
#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "net/usb_ncm.h"

#ifndef USBD_VID
#define USBD_VID 0x2E8A
#endif
#ifndef USBD_PID
#define USBD_PID 0x000C
#endif

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_NCM,
    ITF_NUM_NCM_DATA,
    ITF_NUM_RESET,
    ITF_NUM_TOTAL
};

enum {
    STR_LANGID = 0,
    STR_MANUF,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_NCM,
    STR_MAC,
    STR_RESET
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_NCM_NOTIF 0x83
#define EPNUM_NCM_OUT   0x04
#define EPNUM_NCM_IN    0x84

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
#if PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE && PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR
    .bcdUSB = 0x0210,
#else
    .bcdUSB = 0x0200,
#endif
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1
};

static uint8_t const desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STR_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NCM, STR_NCM, STR_MAC, EPNUM_NCM_NOTIF, 64,
                           EPNUM_NCM_OUT, EPNUM_NCM_IN, CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU),
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, STR_RESET),
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_config;
}

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
static char mac_str[13];
extern uint8_t tud_network_mac_address[6];

static const char *string_desc[] = {
    [STR_MANUF] = "Raspberry Pi",
    [STR_PRODUCT] = "PM220 Pico",
    [STR_SERIAL] = serial_str,
    [STR_CDC] = "Board CDC",
    [STR_NCM] = "PM220 USB Ethernet",
    [STR_MAC] = mac_str,
    [STR_RESET] = "Reset",
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[32];
    if (!serial_str[0]) {
        usb_ncm_ensure_mac();
        pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
        snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
                 tud_network_mac_address[0], tud_network_mac_address[1], tud_network_mac_address[2],
                 tud_network_mac_address[3], tud_network_mac_address[4], tud_network_mac_address[5]);
    }
    uint8_t len;
    if (index == 0) {
        desc[1] = 0x0409;
        len = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0]) || !string_desc[index]) {
            return NULL;
        }
        const char *s = string_desc[index];
        for (len = 0; len < 31 && s[len]; ++len) {
            desc[1 + len] = s[len];
        }
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}
