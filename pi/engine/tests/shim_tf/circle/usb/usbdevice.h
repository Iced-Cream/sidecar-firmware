#pragma once
#include <circle/usb/usb.h>
class CUSBDevice { public: const TUSBDeviceDescriptor* GetDeviceDescriptor() const { return &desc; } TUSBDeviceDescriptor desc = {}; };
