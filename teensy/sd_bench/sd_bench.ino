// SD card benchmark for the Teensy sampler (docs/sampler-plan.md, step 1).
// Read-only: it never writes to the card. Over USB serial it reports:
//  - the card, and a scan of /Samples (folders holding .wav files, files, time)
//  - sequential read speed of the largest file found
//  - 8 streams at once, as 8 voices would read: 4 KB reads round robin over 8
//    files; average and worst time per read, how many took > 1/5/10/20 ms
//  - opening a random file and reading its 44-byte WAV header (a trig on a new
//    sample pays this): average and worst
// Send 'r' to run again.
#include <Arduino.h>
#include <SD.h>

extern "C" uint8_t external_psram_size;   // MB, set at boot (Teensyduino startup)

namespace {

const int kMaxFiles = 400;
const int kPath = 160;
DMAMEM char paths[kMaxFiles][kPath];
int nfiles = 0, nfolders = 0, total_wavs = 0;
uint32_t largest_size = 0;
int largest = -1;

bool is_wav(const char* n) {
  int l = strlen(n);
  return l > 4 && n[0] != '.' && strcasecmp(n + l - 4, ".wav") == 0;
}

void scan(const char* dir, int depth) {
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return;
  bool counted = false;
  for (;;) {
    File e = d.openNextFile();
    if (!e) break;
    char p[kPath];
    snprintf(p, sizeof p, "%s/%s", dir, e.name());
    if (e.isDirectory()) {
      if (depth < 8) scan(p, depth + 1);
    } else if (is_wav(e.name())) {
      ++total_wavs;
      if (!counted) {
        ++nfolders;
        counted = true;
      }
      if (nfiles < kMaxFiles) {
        strncpy(paths[nfiles], p, kPath - 1);
        paths[nfiles][kPath - 1] = 0;
        if (e.size() > largest_size) {
          largest_size = e.size();
          largest = nfiles;
        }
        ++nfiles;
      }
    }
    e.close();
  }
  d.close();
}

uint32_t rnd() {
  static uint32_t s = 12345;
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

DMAMEM uint8_t buf[32768];

void run() {
  Serial.println("--- SD bench ---");
  uint32_t t0 = micros();
  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("no card (SD.begin failed)");
    return;
  }
  Serial.printf("card: begin %lu us, size %.1f GB\n", (unsigned long)(micros() - t0),
                (double)SD.totalSize() / 1e9);
  const char* root = SD.exists("/Samples") ? "/Samples" : (SD.exists("/samples") ? "/samples" : "/");
  nfiles = nfolders = total_wavs = 0;
  largest_size = 0;
  largest = -1;
  t0 = millis();
  scan(root, 0);
  Serial.printf("scan %s: %d folders with .wav, %d .wav files, %lu ms (kept %d paths)\n", root,
                nfolders, total_wavs, (unsigned long)(millis() - t0), nfiles);
  if (nfiles == 0) return;
  for (int i = 0; i < 5 && i < nfiles; ++i) Serial.printf("  e.g. %s\n", paths[(i * 7919) % nfiles]);

  // Sequential read of the largest file.
  {
    File f = SD.open(paths[largest]);
    uint32_t n = 0, t = micros();
    while (n < 16u << 20) {
      int r = f.read(buf, sizeof buf);
      if (r <= 0) break;
      n += r;
    }
    t = micros() - t;
    f.close();
    Serial.printf("sequential: %lu KB of %s in %lu ms = %.2f MB/s\n", (unsigned long)(n >> 10),
                  paths[largest], (unsigned long)(t / 1000), n / (double)t);
  }

  // 8 streams round robin, 4 KB reads (21 ms of 16-bit stereo, 43 ms mono at 48 kHz).
  {
    File f[8];
    for (int v = 0; v < 8; ++v) f[v] = SD.open(paths[rnd() % nfiles]);
    uint32_t reads = 0, worst = 0, over1 = 0, over5 = 0, over10 = 0, over20 = 0;
    uint64_t sum = 0, bytes = 0;
    uint32_t tend = millis() + 5000;
    while ((int32_t)(millis() - tend) < 0) {
      for (int v = 0; v < 8; ++v) {
        uint32_t t = micros();
        int r = f[v].read(buf, 4096);
        if (r < 4096) {   // at the end: another file, as a new trig would
          f[v].close();
          f[v] = SD.open(paths[rnd() % nfiles]);
        }
        t = micros() - t;
        sum += t;
        bytes += r > 0 ? r : 0;
        ++reads;
        if (t > worst) worst = t;
        over1 += t > 1000;
        over5 += t > 5000;
        over10 += t > 10000;
        over20 += t > 20000;
      }
    }
    for (int v = 0; v < 8; ++v) f[v].close();
    Serial.printf("8 streams, 5 s: %lu reads of 4 KB, %.2f MB/s, avg %lu us, worst %lu us; "
                  ">1ms %lu, >5ms %lu, >10ms %lu, >20ms %lu\n",
                  (unsigned long)reads, bytes / 5e6, (unsigned long)(sum / reads),
                  (unsigned long)worst, (unsigned long)over1, (unsigned long)over5,
                  (unsigned long)over10, (unsigned long)over20);
    Serial.printf("  needed for 8 voices of 16-bit stereo at 48 kHz: 1.54 MB/s\n");
  }

  // Open a random file and read its header (a trig on a sample not open yet).
  {
    uint32_t worst = 0;
    uint64_t sum = 0;
    const int n = 200;
    for (int i = 0; i < n; ++i) {
      uint32_t t = micros();
      File f = SD.open(paths[rnd() % nfiles]);
      f.read(buf, 44);
      f.close();
      t = micros() - t;
      sum += t;
      if (t > worst) worst = t;
    }
    Serial.printf("open + header, %d random files: avg %lu us, worst %lu us\n", n,
                  (unsigned long)(sum / n), (unsigned long)worst);
  }
  Serial.printf("PSRAM: %d MB\n", (int)external_psram_size);
  Serial.println("--- done ---");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 8000) {}
  run();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') run();
}
