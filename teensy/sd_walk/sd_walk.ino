// How fast can the Teensy check whether /Samples changed? (docs/sampler-plan.md)
// Read-only. Walks /Samples with SdFat directly (no path opens), hashing each
// entry's name, size, modify time and first cluster (FNV-1a 64) - the listing's
// fingerprint, not the audio. Reports the time, counts, the hash, and whether
// the card is FAT32 or exFAT. Send 'r' to run again (the hash should repeat).
#include <Arduino.h>
#include <SdFat.h>

namespace {

SdFs sd;
uint64_t h;
uint32_t folders, files, wavs, entries;
uint64_t bytes;

void mix(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
}

void walk(FsFile& dir, int depth) {
  FsFile e;
  char name[256];
  ++folders;
  while (e.openNext(&dir, O_RDONLY)) {
    ++entries;
    size_t n = e.getName(name, sizeof name);
    mix(name, n);
    uint16_t d = 0, t = 0;
    e.getModifyDateTime(&d, &t);
    mix(&d, 2);
    mix(&t, 2);
    if (e.isDir()) {
      if (depth < 12) walk(e, depth + 1);
    } else {
      uint64_t sz = e.fileSize();
      uint32_t c = e.firstSector();
      mix(&sz, 8);
      mix(&c, 4);
      ++files;
      bytes += sz;
      if (n > 4 && strcasecmp(name + n - 4, ".wav") == 0) ++wavs;
    }
    e.close();
  }
}

void run() {
  uint32_t t0 = millis();
  if (!sd.begin(SdioConfig(FIFO_SDIO))) {
    Serial.println("no card");
    return;
  }
  Serial.printf("card: %s, begin %lu ms\n", sd.fatType() == FAT_TYPE_EXFAT ? "exFAT" : "FAT32",
                (unsigned long)(millis() - t0));
  FsFile root;
  if (!root.open("/Samples", O_RDONLY) && !root.open("/samples", O_RDONLY)) {
    Serial.println("no /Samples");
    return;
  }
  h = 0xcbf29ce484222325ull;
  folders = files = wavs = entries = 0;
  bytes = 0;
  t0 = millis();
  walk(root, 0);
  uint32_t ms = millis() - t0;
  Serial.printf("walk: %lu ms, %lu folders, %lu entries, %lu files (%lu .wav), %.2f GB\n",
                (unsigned long)ms, (unsigned long)folders, (unsigned long)entries,
                (unsigned long)files, (unsigned long)wavs, bytes / 1e9);
  Serial.printf("listing hash: %08lx%08lx\n", (unsigned long)(h >> 32), (unsigned long)h);
  Serial.printf("reading all of it at the measured 22.9 MB/s would take about %.0f s\n",
                bytes / 22.9e6);
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
