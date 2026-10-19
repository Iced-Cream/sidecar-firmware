// rmp_guard.h - every call into a plugin goes through rmp_guard_call: a fault
// (HardFault/MemManage/BusFault/UsageFault) or an overrun inside it returns to
// the call site with RMP_GUARD_FAULT / RMP_GUARD_TIMEOUT instead of taking the
// Teensy down. The fault handlers and a 2 kHz timer rewrite the exception's
// return address to a trampoline that longjmps back. A hardware watchdog
// (RTWDOG, default 100 ms) restarts a Teensy that hangs outside a plugin; a
// restart caused by it boots in safe mode (no plugins) once.
//
// Rules:
// - Timeouts work only for calls from thread mode (loop()) with BASEPRI 0: the
//   guard's software interrupt has priority 240, the lowest of the 16 levels
//   (4 NVIC priority bits), so it cannot preempt any ISR. From an ISR, faults
//   are still caught, but an endless loop ends in a watchdog reset. Nested
//   calls are refused (RMP_GUARD_BUSY). The 2 kHz IntervalTimer (PIT, priority
//   RMP_GUARD_TICK_PRIO = 128) must be free to run; if no PIT channel was
//   free, rmp_guard_timer_ok() is false and there are no timeouts at all.
// - A plugin that masks interrupts (PRIMASK/BASEPRI) cannot be timed out: an
//   endless loop then ends in an RTWDOG reset. PRIMASK, BASEPRI and FPSCR are
//   restored to their call-time values on every return.
// - After FAULT/TIMEOUT the plugin's half-done work is abandoned: the caller
//   mutes the slot and calls voice_init before any reuse. A timeout that lands
//   inside a host callback (h_log etc.) abandons that too.
// - Faults outside a plugin go to the core's own handler (crash report, USB
//   kept alive); the RTWDOG then resets within its timeout (safe mode next boot).
// - rmp_guard_begin clears SRC_SRSR's WDOG3 bit (sticky until power-on, so safe
//   mode would otherwise repeat); rmp_guard_reset_reason() keeps the whole word.
//   Call it BEFORE printing CrashReport: CrashReport's print clears all of
//   SRC_SRSR (measured: a watchdog reset then reads as safe mode 0).
#pragma once
#include <stdint.h>
enum rmp_guard_result { RMP_GUARD_OK = 0, RMP_GUARD_FAULT, RMP_GUARD_TIMEOUT, RMP_GUARD_BUSY };
#define RMP_GUARD_IRQ_PRIO 240
#define RMP_GUARD_TICK_PRIO 128
void rmp_guard_begin(uint32_t wdog_ms = 100); // handlers, timer, RTWDOG (setup()); wdog_ms 0 = no watchdog
void rmp_guard_feed();                        // RTWDOG refresh (loop(), every pass); no-op after begin(0)
bool rmp_guard_safe_mode();                   // true for the boot after a watchdog reset
bool rmp_guard_timer_ok();                    // false: the 2 kHz timer could not start (no free PIT channel), no timeouts
uint32_t rmp_guard_reset_reason();            // SRC_SRSR as read by rmp_guard_begin (before clearing)
// Run fn(arg) with a cycle limit (0 = none); the timer ends it past the limit
// (resolution one tick, 500 us = 300000 cycles at 600 MHz).
rmp_guard_result rmp_guard_call(void (*fn)(void *), void *arg, uint32_t limit_cycles);
uint32_t rmp_guard_last_cycles();             // the call's cycle count
