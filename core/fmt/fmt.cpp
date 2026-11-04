// core/fmt/fmt.cpp - see fmt.h: the line readers. The atomic writes: atomic.cpp.
#include "fmt.h"

#include <string.h>

namespace core {
namespace fmt {

CORE_COLD int LineReader::line(char* out, int max) {
  int k = 0;
  bool any = false;
  for (;;) {
    if (i == n) {
      if (end) break;
      int r = f.read(buf, sizeof buf);
      if (r <= 0) {
        end = true;
        break;
      }
      n = r, i = 0;
    }
    char c = buf[i++];
    any = true;
    if (c == '\n') break;
    if (c != '\r' && k < max - 1) out[k++] = c;
  }
  out[k] = 0;
  return any ? k : -1;
}

CORE_COLD int next_line(const uint8_t* d, uint32_t n, uint32_t* at, char* out, int max) {
  int k = 0;
  uint32_t i = *at;
  bool any = i < n;
  while (i < n) {
    char c = (char)d[i++];
    if (c == '\n') break;
    if (c != '\r' && k < max - 1) out[k++] = c;
  }
  *at = i;
  out[k] = 0;
  return any ? k : -1;
}

}  // namespace fmt
}  // namespace core
