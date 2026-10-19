// core/tests/test_libs.cpp - the JUNO and DX7 libraries (core/presets/) on a folder standing in for the card:
// /JUNO's patch files and banks, the project's JUNO list; /DX7's banks (a card copy of a Plaits ROM bank found
// as the ROM by its MD5, a card-only bank read from the card), the project's FM list.
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "presets/dx7_lib.h"
#include "presets/juno_lib.h"

static void put(const char* path, const std::vector<uint8_t>& b) {
  FILE* f = fopen(core::host_path(path).c_str(), "wb");
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
// A Juno-106 patch dump: F0 41 30 <ch> <patch no> <16 sliders> <2 switch bytes> F7 (24 bytes).
static void dump(std::vector<uint8_t>& b, int no, int first) {
  b.insert(b.end(), {0xf0, 0x41, 0x30, 0x00, (uint8_t)no});
  for (int i = 0; i < 18; ++i) b.push_back((uint8_t)((first + i) & 0x7f));
  b.push_back(0xf7);
}

int main() {
  char tmpl[] = "/tmp/core-libs-XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::Fs fs;
  CHECK(fs.begin() && fs.mkdir("/JUNO") && fs.mkdir("/DX7/Sub"));
  {
    std::vector<uint8_t> one, bank;
    dump(one, 5, 10);
    put("/JUNO/Pad.syx", one);
    dump(bank, 0, 20); dump(bank, 1, 30); dump(bank, 64, 40);
    put("/JUNO/Fac.syx", bank);
    jlib::Lib j;
    j.begin(&fs, core::log);
    CHECK(j.folders() == 2 && j.folder(1).bank && !strcmp(j.folder(1).name, "Fac"));
    uint16_t sub[8];
    int ns;
    CHECK(j.count(0, sub, 8, &ns) == 2 && ns == 1 && sub[0] == 1);   // the bank, then one patch
    char nm[24];
    uint8_t b[18];
    CHECK(j.patch(0, 0, nm, b) && !strcmp(nm, "Pad") && b[0] == 10 && b[17] == 27);
    CHECK(j.count(1, sub, 8, &ns) == 3 && ns == 0);
    CHECK(j.patch(1, 0, nm, nullptr) && !strcmp(nm, "A11 Fac"));
    CHECK(j.patch(1, 2, nm, b) && !strcmp(nm, "B11 Fac") && b[0] == 40);
    j.load_list("PROJ");
    CHECK(j.listed() == 0);
    CHECK(j.add(1, 1) == 0 && j.listed() == 1 && !strcmp(j.slot(0).name, "A12 Fac") && j.slot(0).b[0] == 30);
    CHECK(j.add(1, 1) == 0 && j.add(0, 0) == 1 && j.listed() == 2);
    jlib::Lib j2;   // the list is on the card: /.rytm/juno/PROJ.txt
    j2.begin(&fs, core::log);
    j2.load_list("PROJ");
    CHECK(j2.listed() == 2 && !strcmp(j2.slot(1).name, "Pad") && j2.slot(1).b[17] == 27);
  }
  {
    std::vector<uint8_t> rom = {0xf0, 0x43, 0x00, 0x09, 0x20, 0x00};
    rom.insert(rom.end(), plaits::syx_bank_0, plaits::syx_bank_0 + 4096);
    rom.push_back(0);
    rom.push_back(0xf7);
    put("/DX7/rom0.syx", rom);
    std::vector<uint8_t> mine(plaits::syx_bank_1, plaits::syx_bank_1 + 4096);
    mine[0] ^= 1;   // not a ROM bank any more
    put("/DX7/Sub/mine.syx", mine);
    dx7::Lib d;
    d.begin(&fs, core::log);
    CHECK(d.nodes() == 4);   // /DX7, its bank and folder, the folder's bank
    uint16_t ch[8];
    CHECK(d.children(0, ch, 8) == 2);
    int krom = d.node(ch[0]).bank ? ch[0] : ch[1], ksub = d.node(ch[0]).bank ? ch[1] : ch[0];
    CHECK(!strcmp(d.node(krom).name, "rom0") && d.children(ksub, ch, 8) == 1);
    int kmine = ch[0];
    char nm[11], want[11];
    CHECK(d.name(krom, 3, nm));
    memcpy(want, plaits::syx_bank_0 + 3 * 128 + 118, 10);
    want[10] = 0;
    CHECK(!memcmp(nm, want, 10));
    d.load_list("PROJ");
    CHECK(d.listed() == 0);
    CHECK(d.add(krom, 3) == 0 && d.add(kmine, 0) == 1 && d.listed() == 2);
    uint8_t v[128];
    CHECK(d.voice(0, v) && !memcmp(v, plaits::syx_bank_0 + 3 * 128, 128));
    CHECK(d.voice(1, v) && v[0] == (plaits::syx_bank_1[0] ^ 1));
    dx7::Lib d2;   // the list again from the card: the ROM bank found by its MD5
    d2.begin(&fs, core::log);
    d2.load_list("PROJ");
    CHECK(d2.listed() == 2 && d2.slot(0).bank == -2 && d2.voice(1, v) && v[0] == (plaits::syx_bank_1[0] ^ 1));
  }
  printf("test_libs: PASS\n");
  return 0;
}
