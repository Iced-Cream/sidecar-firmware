#pragma once
#include <circle/types.h>
class CUSBHCIRootPort {
 public:
  virtual ~CUSBHCIRootPort() {}
  virtual boolean ReScanDevices() = 0;
  virtual boolean RemoveDevice() { ++removed; return TRUE; }
  int removed = 0;
};
