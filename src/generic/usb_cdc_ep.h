#ifndef __GENERIC_USB_CDC_EP_H
#define __GENERIC_USB_CDC_EP_H

#include "autoconf.h" // CONFIG_STM32F4_GD32_USB_INIT_WORKAROUND

// Default USB endpoint ids
#if CONFIG_STM32F4_GD32_USB_INIT_WORKAROUND
enum {
    // Match the GD32F4 CDC-ACM reference layout (IN1, CMD IN2, OUT3).
    USB_CDC_EP_BULK_IN = 1,
    USB_CDC_EP_BULK_OUT = 3,
    USB_CDC_EP_ACM = 2,
};
#else
enum {
    USB_CDC_EP_BULK_IN = 1,
    USB_CDC_EP_BULK_OUT = 2,
    USB_CDC_EP_ACM = 3,
};
#endif

// Default endpoint sizes
enum {
    // Use 64-byte EP0 on GD32 workaround builds to match common FS behavior.
    USB_CDC_EP0_SIZE = CONFIG_STM32F4_GD32_USB_INIT_WORKAROUND ? 64 : 16,
    USB_CDC_EP_ACM_SIZE = 8,
    USB_CDC_EP_BULK_OUT_SIZE = 64,
    USB_CDC_EP_BULK_IN_SIZE = 64,
};

#endif // usb_cdc_ep.h
