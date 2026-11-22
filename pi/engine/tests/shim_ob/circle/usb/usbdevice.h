#pragma once
#include <circle/usb/usb.h>
#include <circle/usb/usbhcirootport.h>
class CUSBStandardHub;
enum { USBSpeedLow, USBSpeedFull, USBSpeedHigh, USBSpeedSuper, USBSpeedUnknown };
class CUSBDevice {
 public:
  const TUSBConfigurationDescriptor* GetConfigurationDescriptor() { return (const TUSBConfigurationDescriptor*)config; }
  const TUSBDeviceDescriptor* GetDeviceDescriptor() const { return &desc; }
  unsigned GetSpeed() { return speed; }
  CUSBHCIRootPort* GetParentRootPort() const { return root; }
  CUSBStandardHub* GetParentHub() const { return hub; }
  unsigned GetParentHubPortIndex() const { return hub_port; }
  boolean RemoveDevice() { ++removes; return TRUE; }   // (the test deletes the function itself)
  int removes = 0;
  CUSBStandardHub* hub = nullptr;
  unsigned hub_port = 0;
  unsigned speed = USBSpeedHigh;
  CUSBHCIRootPort* root = nullptr;
  const u8* config = nullptr;
  TUSBDeviceDescriptor desc = {};
};
