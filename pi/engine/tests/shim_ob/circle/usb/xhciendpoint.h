#pragma once
#define XHCI_EP_MAX_URBS 16
class CXHCIEndpoint {
 public:
  unsigned GetPendingURBs() { return 0; }
  unsigned GetEPState() { return state; }
  bool ResetFromHalted() { ++resets; if (!reset_ok) return false; state = 1; return true; }
  bool IsValid() { return s_valid; }
  static inline bool s_valid = true;   // false: the controller refused the endpoint
  unsigned state = 1;                  // 1 running; 2 halted (usb=mirror's tests set it)
  bool reset_ok = true;
  int resets = 0;
};
