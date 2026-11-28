// pi/engine/code_mmu.h - shipped code's board side on the Pi (core/code/code.h Board): Circle maps everything above
// the kernel's text read-write and never executable (PXN), so the code arena's pages are changed in place in Circle's
// own level-3 table (64 KB pages): writable to place a blob, writable + executable while its constructors run, then
// read-only + executable; back to writable, never executable, when it is freed. Caches and TLBs follow on every core.
#ifndef _code_mmu_h
#define _code_mmu_h
namespace core { namespace code { struct Board; } }

const core::code::Board *code_mmu_board (void);

#endif
