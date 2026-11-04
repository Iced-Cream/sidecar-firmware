// core/tests/bench/trace_steps.c - every instruction of a function's calls, single-stepped (Linux AArch64, a static binary):
//   trace_steps OUT.u32 FUNC_ADDR(hex) SKIP COUNT prog args...
// Calls SKIP .. SKIP + COUNT of the function at FUNC_ADDR: the PCs, as u32, to OUT. For a72_model.py: the exact path a build
// runs, which an Apple core's clock cannot show.
#include <elf.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static void regs(pid_t p, struct { uint64_t r[31], sp, pc, ps; } *g) {
  struct iovec v = {g, sizeof *g};
  if (ptrace(PTRACE_GETREGSET, p, (void*)NT_PRSTATUS, &v)) perror("regs"), exit(1);
}
int main(int ac, char** av) {
  FILE* f = fopen(av[1], "wb");
  uint64_t fn = strtoull(av[2], 0, 16);
  long skip = atol(av[3]), count = atol(av[4]);
  pid_t p = fork();
  if (!p) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    execv(av[5], av + 5);
    _exit(127);
  }
  int st;
  waitpid(p, &st, 0);
  long orig = ptrace(PTRACE_PEEKTEXT, p, (void*)fn, 0);
  long brk = (orig & ~0xffffffffL) | 0xd4200000L;   // brk #0
  ptrace(PTRACE_POKETEXT, p, (void*)fn, (void*)brk);
  long calls = 0;
  struct { uint64_t r[31], sp, pc, ps; } g;
  for (;;) {
    ptrace(PTRACE_CONT, p, 0, 0);
    waitpid(p, &st, 0);
    if (WIFEXITED(st) || WIFSIGNALED(st)) break;
    if (WSTOPSIG(st) != SIGTRAP) continue;
    regs(p, (void*)&g);
    if (g.pc != fn) break;
    uint64_t lr = g.r[30];
    ptrace(PTRACE_POKETEXT, p, (void*)fn, (void*)orig);
    int rec = calls >= skip && calls < skip + count;
    if (rec) {
      for (;;) {
        regs(p, (void*)&g);
        if (g.pc == lr) break;
        uint32_t pc = (uint32_t)g.pc;
        fwrite(&pc, 4, 1, f);
        ptrace(PTRACE_SINGLESTEP, p, 0, 0);
        waitpid(p, &st, 0);
      }
    } else {
      ptrace(PTRACE_SINGLESTEP, p, 0, 0);
      waitpid(p, &st, 0);
    }
    ptrace(PTRACE_POKETEXT, p, (void*)fn, (void*)brk);
    if (++calls >= skip + count) {
      ptrace(PTRACE_POKETEXT, p, (void*)fn, (void*)orig);
      ptrace(PTRACE_KILL, p, 0, 0);
      break;
    }
  }
  fclose(f);
  printf("%ld calls\n", calls);
  return 0;
}
