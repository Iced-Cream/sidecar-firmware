// core/tests/test_host_version.cpp - a Sidecar version with a 4th number (0.5.1.1, the fixes release between 0.5.1 and
// 0.5.2) against the plugins: a package's version and compatible_from are three bytes (major<<16 | minor<<8 | patch,
// tools/rmp_pack.py), so the host's own version is read the same way and the 4th number is left out: 0.5.1.1 is 0.5.1
// to a plugin. A package that says compatible_from 0.5.1 passes the header check and its range holds the host.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "rmp_api.h"
#include "rmp_format.h"
#include "rmp_loader.h"
#include "version.h"

static uint32_t pack3(const char* v) {   // "a.b.c[.d][-pre]" -> a << 16 | b << 8 | c
  unsigned a = 0, b = 0, c = 0;
  CHECK(sscanf(v, "%u.%u.%u", &a, &b, &c) == 3 && a < 256 && b < 256 && c < 256);
  return a << 16 | b << 8 | c;
}

int main() {
  CHECK(pack3("0.5.1.1") == (0u << 16 | 5u << 8 | 1u) && pack3("0.5.1.1") == pack3("0.5.1"));
  CHECK(pack3("0.5.2") > pack3("0.5.1.1"));
  const uint32_t host = pack3(SIDECAR_VERSION);   // whatever VERSION says, 3 or 4 numbers

  // a minimal ARM32 package: the image holds the plugin table, no relocations, no manifest
  uint8_t pkg[RMP_HEADER_BYTES + RMP_PLUGIN_BYTES];
  memset(pkg, 0, sizeof pkg);
  rmp_header h;
  memset(&h, 0, sizeof h);
  h.magic = RMP_FILE_MAGIC, h.format = RMP_FORMAT_RMP1, h.api = RMP_API_MIN;
  strcpy(h.id, "test.hostver");
  h.version = pack3("0.5.1"), h.compatible_from = pack3("0.5.1");
  h.image_size = RMP_PLUGIN_BYTES;
  memcpy(pkg, &h, sizeof h);
  rmp_info info;
  CHECK(rmp_verify_header_any(pkg, sizeof pkg, &info) == RMP_OK);
  CHECK(info.compatible_from == pack3("0.5.1") && info.compatible_from <= host && info.version <= host);
  printf("test_host_version: PASS (host %s)\n", SIDECAR_VERSION);
  return 0;
}
