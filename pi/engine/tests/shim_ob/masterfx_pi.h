#pragma once
#include <stdint.h>
namespace masterfx_pi {
inline void Process(int32_t (*)[12], unsigned) {}
inline bool WantStems() { return false; }
inline void Stems(const int32_t (*)[8], unsigned) {}
inline void Reset() {}
}
