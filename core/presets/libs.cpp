// core/presets/libs.cpp - the JUNO library's one array of patch file names (juno_lib.h declares it).
#include "juno_lib.h"
#if CORE_MSAMP
#include "../fmt/fmt.h"
namespace core {
bool list_post(const char* path, const char* text, uint32_t n) { return fmt::post_queue(path, text, n); }
bool list_replace(const char* path, const char* text, uint32_t n) { return fmt::write_atomic(path, text, n); }
}  // namespace core
#endif

namespace jlib {
CORE_RAM2 Name names_[kNames];
}  // namespace jlib
