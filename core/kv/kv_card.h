// core/kv/kv_card.h - the record store's Backend on the board's card (core::File / core::Fs, core/fmt's
// write_atomic): the card core's only.
#pragma once
#include "kv.h"

namespace core {
namespace kv {

struct CardBackend : Backend {
  bool write_atomic(const char* path, const uint8_t* d, uint32_t n) override;
  int read(const char* path, uint8_t* d, uint32_t cap) override;
  bool remove(const char* path) override;
  bool list(const char* dir, void (*each)(const char* name, void* ctx), void* ctx) override;
};

}  // namespace kv
}  // namespace core
