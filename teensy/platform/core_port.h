// teensy/platform/core_port.h - the Teensy 4.1's port of the engine core (core/platform.h): SdFat files, program
// flash for cold code, RAM2 for bulk data, format strings in flash. teensy/setup_core_lib.sh copies it into
// libraries/RytmCore/src.
#pragma once
#include <Arduino.h>
#include <SdFat.h>

#define CORE_COLD FLASHMEM
#define CORE_RAM2 DMAMEM
#define CORE_STR(s) PSTR(s)

namespace core {

using File = FsFile;   // every member core/platform.h lists is FsFile's own
constexpr int kRead = O_RDONLY;
constexpr int kWrite = O_WRONLY | O_CREAT | O_TRUNC;
constexpr int kAppend = O_WRONLY | O_CREAT | O_APPEND;
constexpr int kReadWrite = O_RDWR | O_CREAT | O_TRUNC;

// The card: SdFat's SdFs as before (mkdir, rename, remove, exists, card() for raw sector reads), plus the core's
// begin() and fingerprint. begin() hides SdFat's begin overloads (with them in scope a bare begin() would be
// ambiguous with begin(SdCsPin_t csPin = SS)); write SdFs::begin(...) for one of those.
class Fs : public SdFs {
 public:
  bool begin();
  uint64_t tree_hash(const char* path, uint32_t* entries, uint32_t* hidden);
  const char* type_name() const { return fatType() == FAT_TYPE_EXFAT ? "exFAT" : "FAT32"; }
};

// The clocks, inline: the render loop reads them on the audio path (no call, no out-of-line ITCM bytes).
inline uint32_t now_us() { return micros(); }
inline uint32_t now_ms() { return millis(); }
inline uint32_t cycles() { return ARM_DWT_CYCCNT; }

// voiceloop's log line writer (plain text or 'L' framed): core::log formats, the sink writes.
void set_log_sink(void (*sink)(const char* line, int n));

}  // namespace core
