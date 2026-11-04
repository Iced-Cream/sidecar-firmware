// pi/engine/tests/test_presetweb.cpp - the web page's JUNO / 6-OP FM lists (presetweb.cpp, GET /presets) on the core's Mac
// port: before the index is walked (a folder: not ready, the project's list is there: the built-ins), the walked index's
// roots (FACTORY / ROM on top, banks and folders flagged, their child numbers), a window (o, n), a name with a quote
// escaped, the arguments (kind, folder out of range, defaults), a buffer too small, the last add's word.
// make -C pi/engine/tests (make -C core engines lib first).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../presetweb.h"
#include "check.h"
#include "core_port.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "presets/preset_index.h"

using namespace core;

static uint64_t g_us = 50000000;
static std::vector<char> g_out(64 * 1024);

static std::string get(const char* params, int cap = 0) {
  int n = presetweb::json(params, g_out.data(), cap ? cap : (int)g_out.size());
  return std::string(g_out.data(), n > 0 ? n : 0);
}
static bool balanced(const std::string& s) {   // brackets and quotes balance (the JSON's shape; the page parses it)
  int depth = 0;
  bool str = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (str) {
      if (c == '\\') ++i;
      else if (c == '"') str = false;
      continue;
    }
    if (c == '"') str = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') { if (--depth < 0) return false; }
  }
  return depth == 0 && !str;
}
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
static void put(const std::string& p, const std::vector<uint8_t>& d) {
  CHECK(!system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()));
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(d.data(), 1, d.size(), f);
  fclose(f);
}
static std::vector<uint8_t> dump(int code, uint8_t seed) {   // a Juno-106 patch dump, 24 bytes
  std::vector<uint8_t> m = {0xf0, 0x41, 0x30, 0, (uint8_t)code};
  for (int i = 0; i < 18; ++i) m.push_back((uint8_t)((seed + i) & 0x7f));
  m.push_back(0xf7);
  return m;
}
static void serve() {
  host_card_core(0);
  cardq::service();
  host_card_core(3);
}

int main() {
  // before the walk: a folder is not ready, the project's list (nothing listed: the built-ins) is
  std::string j = get("k=juno&f=0");
  CHECK(balanced(j) && has(j, "{\"ready\":false,") && !has(j, "\"rows\""));
  j = get("k=fm");
  CHECK(balanced(j) && has(j, "\"ready\":true") && has(j, "\"k\":\"fm\"") && has(j, "\"f\":-1") && has(j, "\"total\":96,\"up\":-1,\"title\":\"FM BUILT-IN\""));
  CHECK(has(j, "\"builtin\":true") && has(j, "\"counts\":[128,96]") && has(j, "\"msg\":{\"seq\":0,\"text\":\"\"}"));
  j = get("k=juno&n=2");
  CHECK(has(j, "\"total\":128,") && has(j, "\"title\":\"JUNO BUILT-IN\"") && has(j, ",2],[\"") && !has(j, "\"kids\":[-1,-1,-1"));

  char tmpl[] = "/tmp/presetweb-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  put(card + "/JUNO/one.syx", dump(5, 11));
  put(card + "/JUNO/q\"x.syx", dump(6, 21));
  put(card + "/JUNO/sub/two.syx", dump(7, 31));
  put(card + "/JUNO/not.txt", {1, 2, 3});
  CHECK(system(("mkdir -p '" + card + "/DX7'").c_str()) == 0);
  host_set_card(card.c_str());
  host_fake_time(true, g_us);
  cardq::reset();
  cardq::set_worker(true);
  presets::begin();
  for (int i = 0; i < 20000 && !presets::ready(); ++i) {
    presets::step();
    serve();
  }
  CHECK(presets::ready());

  // the JUNO root: FACTORY (a folder: flag 1, its child), the sub folder, the patch files; "not.txt" is not a preset
  j = get("k=juno&f=0&o=0&n=50");
  char gen[40];
  snprintf(gen, sizeof gen, "\"gen\":%u,", (unsigned)presets::gen());
  CHECK(balanced(j) && has(j, "{\"ready\":true,") && has(j, gen) && has(j, "\"k\":\"juno\",\"f\":0,\"o\":0,\"total\":4,\"up\":-1,\"title\":\"JUNO\""));
  CHECK(has(j, "\"rows\":[[\"FACTORY\",1],[\"sub\",1],[\"one\",0],[\"q\\\"x\",0]]"));
  CHECK(!has(j, "not.txt") && has(j, "\"kids\":[2,3,-1,-1") && has(j, "\"full\":false"));
  // a window: the second and third row; a folder's rows, its parent and path
  j = get("k=juno&f=0&o=1&n=2");
  CHECK(balanced(j) && has(j, "\"o\":1,") && has(j, "\"rows\":[[\"sub\",1],[\"one\",0]]"));
  j = get("f=2&k=juno&n=200");
  CHECK(balanced(j) && has(j, "\"total\":128,\"up\":0,\"title\":\"JUNO/FACTORY\"") && has(j, "[\"A11 "));
  // the DX7 root: ROM first; the arguments: another kind, a folder out of range, defaults, a number with junk
  j = get("k=fm&f=1");
  CHECK(balanced(j) && has(j, "\"k\":\"fm\",\"f\":1,") && has(j, "\"rows\":[[\"ROM\",1]"));
  CHECK(has(get("k=fm&f=999"), "{\"ready\":false,") && has(get("k=bogus&f=0"), "\"k\":\"juno\",\"f\":0,\"o\":0,\"total\":4"));
  CHECK(has(get("k=juno&f=0&o=x&n=0"), "\"o\":0,") && has(get("k=juno&f=0&n=1"), "\"rows\":[[\"FACTORY\",1]]"));
  CHECK(has(get(""), "\"f\":-1") && has(get("k=juno&f=-5"), "\"f\":-1"));
  // a buffer too small: nothing (the page asks again with a smaller n)
  CHECK(get("k=juno&f=2&n=200", 300).empty() && get("k=juno", 100).empty());
  printf("test_presetweb: PASS\n");
  return 0;
}
