#pragma once
#include <circle/usb/usbfunction.h>
class CUSBStandardHub : public CUSBFunction {
 public:
  CUSBStandardHub(CUSBDevice* d) : CUSBFunction(d, nullptr) {}
  boolean ReScanDevices() { ++rescans; return TRUE; }
  int rescans = 0;
};
