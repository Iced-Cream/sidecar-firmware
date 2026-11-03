#pragma once
#include <map>
#include <string>
class CKernelOptions {
 public:
  static CKernelOptions* Get() { static CKernelOptions o; return &o; }
  unsigned GetAppOptionDecimal(const char* n, unsigned d) { auto i = num.find(n); return i == num.end() ? d : i->second; }
  const char* GetAppOptionString(const char*, const char* d) { return d; }
  std::map<std::string, unsigned> num;   // what a test sets
};
