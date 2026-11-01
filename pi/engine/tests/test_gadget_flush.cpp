// pi/engine/tests/test_gadget_flush.cpp - pi/circle/gadget_flush.h: the TX FIFO flush of a USB reset, at IRQ level: done in the read the bit clears, no read
// after it, bounded when the core never clears it. circle-gadget-reset.patch's use of it runs only in a kernel.
#include "check.h"
#include "../../circle/gadget_flush.h"

struct Reg {
  unsigned clear_after, reads = 0;
  unsigned Read() { return ++reads > clear_after ? 0u : 0x20u; }
};

int main() {
  Reg now{0};
  CHECK(GadgetBitClears(now, 0x20) && now.reads == 1);
  Reg soon{37};
  CHECK(GadgetBitClears(soon, 0x20) && soon.reads == 38);
  Reg other{1000};
  CHECK(GadgetBitClears(other, 0x10) && other.reads == 1);     // another bit
  Reg stuck{0xFFFFFFFFu};
  CHECK(!GadgetBitClears(stuck, 0x20) && stuck.reads == kGadgetFlushReads);
  printf("gadget_flush: PASS\n");
  return 0;
}
