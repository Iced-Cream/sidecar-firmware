// core/kv/link.h - 51 STORE in the engine: the host link's messages arrive on the control core (the Pi's core 3),
// the records live on the card, which only the card core (core 0) touches. from_host() queues a store op to the card
// core and runs a lock op (core/locks) at once; card_step() runs queued store ops against the card (core 0's loop,
// beside cardq::service); host_step() sends the answers to the Rytm (the control core's idle, core::to_rytm).
// Without a card core (the Mac core, a test) card_step() is called from the same loop. Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace kv {
namespace link {

constexpr int kMsgMax = 160;   // a WRITE chunk: 8 header + 6 + 128 data + F7
bool from_host(const uint8_t* m, int n);   // control core: a 51 message; false: not 51 / queue full (counted)
int card_step(int max_ops);                // card core: up to max_ops queued ops run; returns how many
int host_step(int max_msgs);               // control core: answers sent; returns how many
void hello();                              // the link restarted: the lock sets forgotten, the store index kept (a
                                           // store HELLO does the same in from_host; the Rytm's 35 does not)
// The generic engine: 51..53 through core::gen::set_store, a link reset (another box, not the Rytm's
// 35) through gen::on_link, the locks on
// the clock and the processor slots (locks::attach). The platform then calls card_step (card core) and host_step
// (control idle). Boot, once, after gen::begin(). False: a listener had no room.
bool attach_generic();

// The project's name as the companion last sent it (04 NAME, control core): "" before one; *project its ID (may be 0).
// on_name: called (control core) each time it changes; false: no room (2 listeners).
constexpr int kNameMax = 16;
const char* project_name(uint32_t* project = nullptr);
using NameFn = void (*)(const char* name, uint32_t project);
bool on_name(NameFn f);

struct Stats { uint32_t in, out, dropped_in, dropped_out, ops, names; };
Stats stats();

}  // namespace link
}  // namespace kv
}  // namespace core
