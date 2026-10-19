// The fast version of sd_walk: read /Samples' directory entries raw (32-byte
// FAT32 entries: 8.3 name, attributes, size, first cluster, modify time) without
// opening each file - only folders are opened. Hashes those (FNV-1a 64), skipping
// long-name entries, deleted ones and "." / "..". Read-only. Send 'r' to rerun.
#include <Arduino.h>
#include <SdFat.h>

namespace {

SdFat32 sd;
uint64_t h;
uint32_t folders, files, entries;
uint64_t bytes;

void mix(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
}

void walk(File32& dir, int depth) {
  DirFat_t d;
  ++folders;
  dir.rewind();
  for (;;) {
    uint32_t pos = dir.curPosition();
    if (dir.read(&d, sizeof d) != (int)sizeof d) break;
    if (d.name[0] == 0) break;                       // end of directory
    if (d.name[0] == 0xe5) continue;                 // deleted
    if ((d.attributes & 0x0f) == 0x0f) continue;     // long-name part
    if (d.name[0] == '.') continue;                  // "." and ".."
    ++entries;
    mix(&d, sizeof d);                               // name, attr, times, cluster, size
    if (d.attributes & 0x10) {
      if (depth < 12) {
        File32 sub;
        if (sub.open(&dir, pos / 32, O_RDONLY)) {
          walk(sub, depth + 1);
          sub.close();
        }
        dir.seekSet(pos + 32);
      }
    } else if (!(d.attributes & 0x08)) {
      ++files;
      bytes += getLe32(d.fileSize);
    }
  }
}

void run() {
  if (!sd.begin(SdioConfig(FIFO_SDIO))) {
    Serial.println("no card / not FAT32");
    return;
  }
  File32 root;
  if (!root.open("/Samples", O_RDONLY) && !root.open("/samples", O_RDONLY)) {
    Serial.println("no /Samples");
    return;
  }
  h = 0xcbf29ce484222325ull;
  folders = files = entries = 0;
  bytes = 0;
  uint32_t t0 = millis();
  walk(root, 0);
  uint32_t ms = millis() - t0;
  Serial.printf("raw walk: %lu ms, %lu folders, %lu entries, %lu files, %.2f GB\n",
                (unsigned long)ms, (unsigned long)folders, (unsigned long)entries,
                (unsigned long)files, bytes / 1e9);
  Serial.printf("listing hash: %08lx%08lx\n", (unsigned long)(h >> 32), (unsigned long)h);
  root.close();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 8000) {}
  run();
  run();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') run();
}
