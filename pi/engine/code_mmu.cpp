// pi/engine/code_mmu.cpp - see code_mmu.h.
#include "code_mmu.h"
#include "code/code.h"

#include <circle/armv8mmu.h>
#include <circle/types.h>

namespace {

constexpr u64 kPage = ARMV8MMU_LEVEL3_PAGE_SIZE;	// 64 KB
constexpr u64 kAddrMask = 0x0000FFFFFFFF0000ull;	// a descriptor's next-level / output address, [47:16]
constexpr u64 kAP = 3ull << 6;				// AP[2:1]
constexpr u64 kAPRO = (u64) ATTRIB_AP_RO_EL1 << 6;
constexpr u64 kPXN = 1ull << 53;

// the level-3 descriptor that maps va (Circle: TTBR0 -> one level-2 table of 512 MB entries -> level-3 tables);
// null when va is not mapped by a table entry
volatile u64 *Descriptor (u64 va)
{
	u64 ttbr;
	asm volatile ("mrs %0, ttbr0_el1" : "=r" (ttbr));
	const volatile u64 *pL2 = (const volatile u64 *) (ttbr & kAddrMask);
	const u64 l2 = pL2[va >> 29];
	if ((l2 & 3) != 3)
		return 0;				// invalid or a block: not Circle's layout
	volatile u64 *pL3 = (volatile u64 *) (l2 & kAddrMask);
	volatile u64 *pDesc = &pL3[(va >> 16) & (ARMV8MMU_TABLE_ENTRIES - 1)];
	if ((*pDesc & 3) != 3)
		return 0;
	return pDesc;
}

// data cache clean to the point of unification and instruction cache invalidate over [p, p + n), every core
void SyncCode (u64 p, u64 n)
{
	u64 ctr;
	asm volatile ("mrs %0, ctr_el0" : "=r" (ctr));
	const u64 dline = 4ull << ((ctr >> 16) & 0xF), iline = 4ull << (ctr & 0xF);
	for (u64 a = p & ~(dline - 1); a < p + n; a += dline)
		asm volatile ("dc cvau, %0" :: "r" (a) : "memory");
	asm volatile ("dsb ish" ::: "memory");
	for (u64 a = p & ~(iline - 1); a < p + n; a += iline)
		asm volatile ("ic ivau, %0" :: "r" (a) : "memory");
	asm volatile ("dsb ish\n isb" ::: "memory");
}

bool Protect (void *p, size_t n, core::code::Prot prot)
{
	const u64 base = (u64) p;
	if ((base & (kPage - 1)) || (n & (kPage - 1)))
		return false;
	for (u64 va = base; va < base + n; va += kPage)	// every page first: all or nothing
		if (!Descriptor (va))
			return false;
	for (u64 va = base; va < base + n; va += kPage)
	{
		volatile u64 *pDesc = Descriptor (va);
		u64 d = *pDesc & ~(kAP | kPXN);
		if (prot == core::code::kProtRX)
			d |= kAPRO;
		else if (prot == core::code::kProtRW)
			d |= kPXN;
		*pDesc = d;
		asm volatile ("dc civac, %0" :: "r" ((u64) pDesc) : "memory");	// the table walk sees it
	}
	asm volatile ("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
	if (prot != core::code::kProtRW)
		SyncCode (base, n);
	return true;
}

const core::code::Board kBoard = {Protect};

}  // namespace

const core::code::Board *code_mmu_board (void)
{
	return &kBoard;
}
