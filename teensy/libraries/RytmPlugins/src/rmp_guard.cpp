// rmp_guard.cpp - see rmp_guard.h.
//
// Faults: the four fault vectors point at rmp_fault_isr. A fault taken while a
// guarded call runs, in the caller's context (the stacked IPSR equals the one at
// call time), gets its stacked PC rewritten to rmp_guard_trampoline, which runs
// after the exception return and longjmps to the call site. Anything else is
// chained to the handler that was there before (the core's
// unused_interrupt_vector: crash report, USB alive, reboot after 8 s).
//
// Overruns: a 2 kHz IntervalTimer callback cannot reach the plugin's exception
// frame (it runs inside the shared PIT handler), so it only pends a software
// interrupt at the lowest priority. That interrupt is taken on the way back to
// the plugin's context and its naked handler sees the plugin's frame.
//
// The software interrupt is NVIC IRQ 71 (IRQ_Reserved2: no peripheral is wired
// to it). IRQ_SOFTWARE (70) is not used: the Teensy Audio library's
// AudioStream owns it for its update.
//
// Lazy FP stacking: the handlers do no floating point; the extended (FPCA)
// frame is left as is (only PC = frame[6] and xPSR = frame[7] are changed), so
// the FP context the hardware restores is the plugin's own, and the longjmp
// then restores the caller's callee-saved registers.
#include "rmp_guard.h"
#include <Arduino.h>
#include <setjmp.h>
extern "C" void (* volatile _VectorsRam[])(void);

#define RMP_GUARD_IRQ IRQ_Reserved2    /* 71 */

extern "C" { jmp_buf rmp_guard_jb; }   // C name: the naked trampoline refers to it
static volatile uint32_t in_plugin, busy, t_start, t_limit, last_cycles, call_ipsr, saved_primask, saved_basepri, saved_fpscr;
static volatile int result;
static void (*prev_fault[7])(void);    // the handlers of vectors 3..6 before rmp_guard_begin
static IntervalTimer tick;
static bool safe, wdog_on, timer_ok;
static uint32_t srsr;

// Naked: no prologue, so nothing is pushed on the (possibly broken) plugin stack;
// longjmp restores SP and the callee-saved registers from rmp_guard_jb.
extern "C" __attribute__((naked, noreturn, used)) void rmp_guard_trampoline() {
  asm volatile(
      "ldr r0, =rmp_guard_jb\n"
      "movs r1, #1\n"
      "b longjmp\n"
      ".ltorg\n");
}

// Is this stacked frame the guarded call's context?
static inline bool ours(const uint32_t *frame) { return in_plugin && (frame[7] & 0x1FFu) == call_ipsr; }

// Send the frame's return to the trampoline: PC (halfword-aligned, no Thumb bit),
// xPSR keeps T (24), the stack-realign bit (9) and the exception number; IT/ICI
// and the flags are cleared.
static void redirect(uint32_t *frame, int why) {
  result = why;
  frame[6] = (uint32_t)&rmp_guard_trampoline & ~1u;
  frame[7] &= (1u << 24) | (1u << 9) | 0x1FFu;
  in_plugin = 0;
}

// Returns 0 when handled (exception return to the trampoline) or the previous
// handler to chain to.
extern "C" uint32_t rmp_fault_c(uint32_t *frame) {
  uint32_t ipsr;
  asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  uint32_t cfsr = SCB_CFSR;
  // a fault while stacking or unstacking (MUNSTKERR 3, MSTKERR 4, UNSTKERR 11,
  // STKERR 12): the frame is not trustworthy - checked before reading it
  const uint32_t stack_errs = (1u << 3) | (1u << 4) | (1u << 11) | (1u << 12);
  if (!(cfsr & stack_errs) && ours(frame)) {
    SCB_CFSR = cfsr;                   // write-1-to-clear
    SCB_HFSR = SCB_HFSR;               // FORCED, if a fault escalated to HardFault
    redirect(frame, RMP_GUARD_FAULT);
    return 0;
  }
  ipsr &= 0x1FFu;
  return (uint32_t)(ipsr >= 3 && ipsr <= 6 ? prev_fault[ipsr] : nullptr);
}
extern "C" __attribute__((naked)) void rmp_fault_isr() {
  asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "push {r4, lr}\n"
      "bl rmp_fault_c\n"
      "pop {r4, lr}\n"
      "cbz r0, 1f\n"
      "bx r0\n"                          // chain: lr (EXC_RETURN) and the stack as on entry
      "1: bx lr\n");
}

extern "C" void rmp_sw_c(uint32_t *frame) {
  if (ours(frame) && t_limit && ARM_DWT_CYCCNT - t_start > t_limit) redirect(frame, RMP_GUARD_TIMEOUT);
}
extern "C" __attribute__((naked)) void rmp_sw_isr() {
  asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "b rmp_sw_c\n");
}
static void tick_cb() { if (in_plugin) NVIC_SET_PENDING(RMP_GUARD_IRQ); }

// RTWDOG (WDOG3) on the 32.768 kHz LPO clock (CS.CLK = 1; 32759 Hz measured). The reset reason is
// SRC_SRSR bit 7 (WDOG3_RST_B), sticky until power-on.
#define RMP_WDOG_LPO_HZ 32768u
void rmp_guard_begin(uint32_t wdog_ms) {
  srsr = SRC_SRSR;
  safe = (srsr & SRC_SRSR_WDOG3_RST_B) != 0;
  SRC_SRSR = SRC_SRSR_WDOG3_RST_B;     // write-1-to-clear this bit only (sticky until power-on); CrashReport still sees the rest
  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
  for (int v = 3; v <= 6; ++v) {       // HardFault, MemManage, BusFault, UsageFault
    if (_VectorsRam[v] != rmp_fault_isr) prev_fault[v] = _VectorsRam[v];
    _VectorsRam[v] = rmp_fault_isr;
  }
  SCB_SHCSR |= SCB_SHCSR_MEMFAULTENA | SCB_SHCSR_BUSFAULTENA | SCB_SHCSR_USGFAULTENA;
  _VectorsRam[RMP_GUARD_IRQ + 16] = rmp_sw_isr;
  NVIC_SET_PRIORITY(RMP_GUARD_IRQ, RMP_GUARD_IRQ_PRIO);   // lowest: taken on the way back to the plugin
  NVIC_ENABLE_IRQ(RMP_GUARD_IRQ);
  tick.priority(RMP_GUARD_TICK_PRIO);
  timer_ok = tick.begin(tick_cb, 500); // 2 kHz; false when all four PIT channels are taken: no timeouts then
  tick.priority(RMP_GUARD_TICK_PRIO);
  if (!wdog_ms) return;
  uint64_t toval = ((uint64_t)wdog_ms * RMP_WDOG_LPO_HZ + 500u) / 1000u;
  uint32_t cs = WDOG_CS_CMD32EN | WDOG_CS_CLK(1) | WDOG_CS_EN | WDOG_CS_UPDATE | WDOG_CS_WAIT;
  if (toval > 0xFFFFu) { toval /= 256u; cs |= WDOG_CS_PRES; }
  if (toval > 0xFFFFu) toval = 0xFFFFu;
  CCM_CCGR5 |= CCM_CCGR5_WDOG3(CCM_CCGR_ON);
  __disable_irq();                     // unlock + reconfigure must finish within 255 bus clocks
  if (WDOG3_CS & WDOG_CS_CMD32EN) WDOG3_CNT = 0xD928C520u;
  else { WDOG3_CNT = 0xC520u; WDOG3_CNT = 0xD928u; }
  while (!(WDOG3_CS & WDOG_CS_ULK)) {}
  WDOG3_WIN = 0;
  WDOG3_TOVAL = (uint32_t)toval;
  WDOG3_CS = cs;
  __enable_irq();
  while (!(WDOG3_CS & WDOG_CS_RCS)) {}
  wdog_on = true;
}
void rmp_guard_feed() { if (wdog_on) WDOG3_CNT = 0xB480A602u; }   // the RTWDOG 32-bit refresh word
bool rmp_guard_safe_mode() { return safe; }
bool rmp_guard_timer_ok() { return timer_ok; }
uint32_t rmp_guard_reset_reason() { return srsr; }
uint32_t rmp_guard_last_cycles() { return last_cycles; }

static inline void restore_masks() {
  asm volatile("vmsr fpscr, %0" ::"r"(saved_fpscr) : "memory");
  asm volatile("msr basepri, %0" ::"r"(saved_basepri) : "memory");
  if (saved_primask) { __disable_irq(); } else { __enable_irq(); }
}

rmp_guard_result rmp_guard_call(void (*fn)(void *), void *arg, uint32_t limit) {
  uint32_t pm, bp, ipsr, fpscr;
  asm volatile("mrs %0, primask" : "=r"(pm));
  __disable_irq();
  if (busy) {                          // nested (an ISR while a guarded call runs): refused
    if (!pm) __enable_irq();
    return RMP_GUARD_BUSY;
  }
  busy = 1;
  if (!pm) __enable_irq();
  asm volatile("mrs %0, basepri" : "=r"(bp));
  asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
  saved_primask = pm; saved_basepri = bp; saved_fpscr = fpscr; call_ipsr = ipsr & 0x1FFu;
  result = RMP_GUARD_OK;
  if (setjmp(rmp_guard_jb)) {                    // from the trampoline: in_plugin is already 0
    last_cycles = ARM_DWT_CYCCNT - t_start;
    restore_masks();
    rmp_guard_result r = (rmp_guard_result)result;
    busy = 0;
    return r;
  }
  t_limit = limit; t_start = ARM_DWT_CYCCNT; in_plugin = 1;
  fn(arg);
  asm volatile("dsb\n isb" ::: "memory");   // an imprecise BusFault from the plugin's last stores lands inside the window
  in_plugin = 0;
  last_cycles = ARM_DWT_CYCCNT - t_start;
  restore_masks();
  busy = 0;
  return RMP_GUARD_OK;
}
