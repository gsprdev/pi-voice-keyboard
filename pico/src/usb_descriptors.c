// USB descriptors for the composite device: HID boot keyboard + CDC serial.

#include "pico/unique_id.h"
#include "tusb.h"

// Same IDs as the Pi gadget (pi/gadget-bind.sh)
#define USB_VID 0x1d6b // Linux Foundation
#define USB_PID 0x0104 // Multifunction Composite Gadget

enum {
    ITF_NUM_HID,
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

enum {
    STR_LANGID,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_HID,
    STR_CDC,
};

#define EPNUM_HID       0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT   0x03
#define EPNUM_CDC_IN    0x83

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Interface Association Descriptor, required for CDC in a composite device
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

// Standard boot keyboard report: [modifiers, reserved, key1-6], plus LED output
static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 120),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 1),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STR_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const string_desc[] = {
    [STR_MANUFACTURER] = "gspr.dev",
    [STR_PRODUCT]      = "Voice Keyboard",
    [STR_SERIAL]       = serial_str,
    [STR_HID]          = "Voice Keyboard",
    [STR_CDC]          = "Voice Keyboard Console",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[32];
    size_t len;

    if (index == STR_LANGID) {
        desc_str[1] = 0x0409; // English
        len = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc)) {
            return NULL;
        }
        if (index == STR_SERIAL && !serial_str[0]) {
            pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
        }
        const char *str = string_desc[index];
        for (len = 0; len < TU_ARRAY_SIZE(desc_str) - 1 && str[len]; len++) {
            desc_str[1 + len] = str[len];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
