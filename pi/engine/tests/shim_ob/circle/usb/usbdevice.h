#pragma once
#include <circle/usb/usb.h>
#include <circle/usb/usbhcirootport.h>
enum { USBSpeedLow, USBSpeedFull, USBSpeedHigh, USBSpeedSuper, USBSpeedUnknown };
class CUSBDevice {
 public:
  const TUSBConfigurationDescriptor* GetConfigurationDescriptor() { return (const TUSBConfigurationDescriptor*)config; }
  const TUSBDeviceDescriptor* GetDeviceDescriptor() const { return &desc; }
  unsigned GetSpeed() { return speed; }
  CUSBHCIRootPort* GetParentRootPort() const { return root; }
  unsigned speed = USBSpeedHigh;
  CUSBHCIRootPort* root = nullptr;
  const u8* config = nullptr;
  TUSBDeviceDescriptor desc = {};
};
