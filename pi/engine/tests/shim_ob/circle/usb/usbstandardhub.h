#pragma once
#include <circle/usb/usbfunction.h>
class CUSBStandardHub : public CUSBFunction {
 public:
  CUSBStandardHub(CUSBDevice* d) : CUSBFunction(d, nullptr) {}
  boolean ReScanDevices() { ++rescans; return TRUE; }
  int rescans = 0;
  boolean RemoveDeviceAt(unsigned i) { removed_at = (int) i; ++removes; return remove_ok; }
  int removes = 0, removed_at = -1;
  boolean remove_ok = TRUE;
};
