#pragma once
#define XHCI_EP_MAX_URBS 16
class CXHCIEndpoint {
 public:
  unsigned GetPendingURBs() { return 0; }
  unsigned GetEPState() { return 1; }
  bool IsValid() { return s_valid; }
  static inline bool s_valid = true;   // false: the controller refused the endpoint
};
