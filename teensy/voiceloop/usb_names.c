// The name the computer shows for the Sidecar Mini's own USB port ("USB Serial" is Teensyduino's default). The core's
// usb_desc.c takes these over its weak defaults; the vendor / product IDs stay PJRC's (the web console and the
// Teensy tools find the board by them), only the strings change.
#include "usb_names.h"

#define PRODUCT_NAME     {'S','i','d','e','c','a','r',' ','M','i','n','i'}
#define PRODUCT_NAME_LEN 12

struct usb_string_descriptor_struct usb_string_product_name = {
  2 + PRODUCT_NAME_LEN * 2,
  3,
  PRODUCT_NAME
};
