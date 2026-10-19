// core/tests/test_platform.cpp - the Mac port: files with SdFat's names and meanings, the clock, the log.
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "platform.h"

static std::string g_log;
static void sink(const char* line, int n) { g_log.assign(line, n); }

int main() {
  char tmpl[] = "/tmp/core-card-XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::Fs fs;
  CHECK(fs.begin());
  CHECK(fs.mkdir("/Samples"));
  CHECK(fs.mkdir("/Samples/b"));
  CHECK(fs.mkdir("/Samples/a"));
  CHECK(fs.exists("/Samples/a") && !fs.exists("/Samples/c"));
  {   // write, append, read back by byte and by block, seek
    core::File f;
    CHECK(f.open("/Samples/a/x.txt", core::kWrite));
    CHECK(f.write("hello", 5) == 5);
    CHECK(f.close());
    CHECK(f.open("/Samples/a/x.txt", core::kAppend));
    CHECK(f.write((uint8_t)'!') == 1);
    f.close();
    CHECK(f.open("/Samples/a/x.txt", core::kRead));
    CHECK(f.fileSize() == 6 && f.available() == 6 && !f.isDir());
    CHECK(f.read() == 'h');
    char b[8] = {0};
    CHECK(f.read(b, 8) == 5 && memcmp(b, "ello!", 5) == 0);
    CHECK(f.read() == -1);
    CHECK(f.seekSet(1) && f.curPosition() == 1 && f.read() == 'e');
    core::File g = f;   // a copy: its own position
    CHECK(g.read() == 'l' && f.read() == 'l' && g.curPosition() == 3);
    f.rewind();
    CHECK(f.curPosition() == 0);
  }
  {   // read-write (dx7_lib's index build): write, seek back and read, rewrite the header, write at the end
    core::File f;
    CHECK(f.open("/Samples/a/rw.bin", core::kReadWrite));
    CHECK(f.write("HDR.", 4) == 4 && f.write("abcd", 4) == 4);
    char b[4];
    CHECK(f.seekSet(4) && f.read(b, 4) == 4 && memcmp(b, "abcd", 4) == 0);
    CHECK(f.seekSet(8) && f.write("ef", 2) == 2);
    CHECK(f.seekSet(0) && f.write("HEAD", 4) == 4 && f.curPosition() == 4 && f.fileSize() == 10);
    f.close();
    CHECK(f.open("/Samples/a/rw.bin", core::kReadWrite) && f.fileSize() == 0);   // truncates, as O_TRUNC
    CHECK(f.write("xyz", 3) == 3);
    core::File g = f;   // a copy keeps the contents
    CHECK(g.fileSize() == 3 && g.seekSet(1) && g.read() == 'y');
    f.close();
    g.close();
    char all[16] = {0};
    CHECK(f.open("/Samples/a/rw.bin", core::kRead) && f.read(all, sizeof all) == 3 && memcmp(all, "xyz", 3) == 0);
    f.close();
    CHECK(f.open("/Samples/a/w.bin", core::kWrite) && f.write("1234", 4) == 4);   // kWrite: a write after a seek lands there
    CHECK(f.seekSet(1) && f.write("x", 1) == 1);
    f.close();
    CHECK(f.open("/Samples/a/w.bin", core::kRead) && f.read(all, sizeof all) == 4 && memcmp(all, "1x34", 4) == 0);
    f.close();
    CHECK(fs.remove("/Samples/a/rw.bin") && fs.remove("/Samples/a/w.bin"));
  }
  {   // directories: entries in name order, dirIndex reopens the same entry
    core::File d, e;
    CHECK(d.open("/Samples", core::kRead) && d.isDir());
    char nm[64];
    CHECK(e.openNext(&d, core::kRead) && e.isDir());
    e.getName(nm, sizeof nm);
    CHECK(strcmp(nm, "a") == 0);
    uint32_t ia = e.dirIndex();
    CHECK(e.openNext(&d, core::kRead));
    e.getName(nm, sizeof nm);
    CHECK(strcmp(nm, "b") == 0);
    CHECK(!e.openNext(&d, core::kRead));
    core::File again;
    CHECK(again.open(&d, ia, core::kRead));
    again.getName(nm, sizeof nm);
    CHECK(strcmp(nm, "a") == 0 && again.isDir());
  }
  {   // the fingerprint: stable, changed by a new file, 0 without the folder
    uint32_t n, hid;
    uint64_t h1 = fs.tree_hash("/Samples", &n, &hid), h2 = fs.tree_hash("/Samples", &n, &hid);
    CHECK(h1 && h1 == h2);
    core::File f;
    CHECK(f.open("/Samples/b/._junk", core::kWrite));
    f.close();
    CHECK(fs.tree_hash("/Samples", &n, &hid) == h1 && hid == 1);   // hidden ('.') entries leave it alone
    CHECK(f.open("/Samples/b/y.wav", core::kWrite));
    f.close();
    CHECK(fs.tree_hash("/Samples", &n, &hid) != h1);
    CHECK(fs.tree_hash("/Nope", &n, &hid) == 0);
  }
  {   // rename and remove
    CHECK(fs.rename("/Samples/a/x.txt", "/Samples/a/z.txt"));
    CHECK(fs.exists("/Samples/a/z.txt") && !fs.exists("/Samples/a/x.txt"));
    CHECK(fs.remove("/Samples/a/z.txt") && !fs.exists("/Samples/a/z.txt"));
  }
  {   // clock, cycles, the log sink
    uint32_t t0 = core::now_us(), c0 = core::cycles();
    usleep(2000);
    CHECK(core::now_us() - t0 >= 2000 && core::cycles() - c0 > 0 && core::cycles_per_second() > 0);
    core::host_set_log_sink(sink);
    core::log(CORE_STR("voice %d: %s"), 3, "ready");
    CHECK(g_log == "voice 3: ready");
  }
  srand(1);
  CHECK(rand() == 1481765933);   // newlib's sequence (core/host/newlib_rand.cpp), as on the Teensy
  CHECK(system((std::string("rm -rf '") + tmpl + "'").c_str()) == 0);
  printf("test_platform: PASS\n");
  return 0;
}
