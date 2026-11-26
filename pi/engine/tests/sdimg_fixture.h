// pi/engine/tests/sdimg_fixture.h - a small builder of the SD image's layout (docs/sd-image.md) on an in-memory,
// sparse disk, with a small FAT cap so the disks stay small; and a disk that can lose its writes.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include "sdgrow.h"

namespace fix {

const uint32_t kStart = 8192, kRsv = 32, kSpc = 64;

struct MemDisk : sdgrow::Disk
{
	typedef std::vector<uint8_t> Sec;
	std::map<uint64_t, Sec> m;
	uint64_t total = 0;
	int writes = 0;		// writes accepted
	int cutAfter = -1;	// >= 0: this many writes work, then every write fails (a power cut)
	bool lie = false;	// writes "succeed" and store nothing
	uint64_t sectors () const override { return total; }
	static void aligned (const void *p) { if (((uintptr_t) p & 15) != 0) { fprintf (stderr, "sector buffer %p not 16-byte aligned\n", p); exit (1); } }
	bool read (uint64_t lba, void *p) override
	{
		aligned (p);
		if (lba >= total) return false;
		auto it = m.find (lba);
		if (it == m.end ()) memset (p, 0, 512); else memcpy (p, it->second.data (), 512);
		return true;
	}
	bool write (uint64_t lba, const void *p) override
	{
		aligned (p);
		if (lba >= total || (cutAfter >= 0 && writes >= cutAfter)) return false;
		writes++;
		if (!lie) m[lba] = Sec ((const uint8_t *) p, (const uint8_t *) p + 512);
		return true;
	}
	void put (uint64_t lba, uint32_t off, const void *p, uint32_t n)
	{
		Sec &s = m[lba];
		if (s.empty ()) s.assign (512, 0);
		memcpy (&s[off], p, n);
	}
	void put32 (uint64_t lba, uint32_t off, uint32_t v) { uint8_t b[4] = {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)}; put (lba, off, b, 4); }
	void put16 (uint64_t lba, uint32_t off, uint32_t v) { uint8_t b[2] = {(uint8_t) v, (uint8_t) (v >> 8)}; put (lba, off, b, 2); }
	void put8 (uint64_t lba, uint32_t off, uint32_t v) { uint8_t b = (uint8_t) v; put (lba, off, &b, 1); }
	uint32_t get32 (uint64_t lba, uint32_t off) { uint8_t s[512]; read (lba, s); return s[off] | s[off + 1] << 8 | s[off + 2] << 16 | (uint32_t) s[off + 3] << 24; }
	uint32_t get16 (uint64_t lba, uint32_t off) { uint8_t s[512]; read (lba, s); return s[off] | s[off + 1] << 8; }
};

struct Spec
{
	uint64_t cardSectors = 2ull * 1024 * 1024;	// 1 GiB
	uint32_t volClusters = 2000;			// the image's volume: about 64 MB
	uint32_t capClusters = 20000;			// what the FATs are sized for (the real image: 512 GiB)
};

inline uint32_t fatsz (uint32_t capClusters) { return ((uint64_t) (capClusters + 2) * 4 + 511) / 512; }
inline uint64_t overhead (const Spec &s) { return kRsv + 2ull * fatsz (s.capClusters); }
inline uint64_t volSectors (const Spec &s) { return overhead (s) + (uint64_t) s.volClusters * kSpc; }

// The partition, boot sector and backup, FSInfo and backup, FATs' first sector, a root label entry, one data marker.
inline void build (MemDisk &d, const Spec &s)
{
	d.total = s.cardSectors;
	uint32_t fz = fatsz (s.capClusters);
	uint32_t vol = (uint32_t) volSectors (s);
	d.put32 (0, 440, 0x51DECA4D);					// the disk signature
	d.put8 (0, 446, 0); d.put8 (0, 446 + 4, 0x0C);
	d.put32 (0, 446 + 8, kStart); d.put32 (0, 446 + 12, vol);
	d.put16 (0, 510, 0xAA55);
	for (uint32_t base : {kStart, kStart + 6})
	{
		d.put (base, 3, "SIDECAR ", 8);
		d.put16 (base, 11, 512); d.put8 (base, 13, kSpc); d.put16 (base, 14, kRsv); d.put8 (base, 16, 2);
		d.put16 (base, 17, 0); d.put16 (base, 19, 0); d.put8 (base, 21, 0xF8); d.put16 (base, 22, 0);
		d.put32 (base, 28, kStart); d.put32 (base, 32, vol); d.put32 (base, 36, fz);
		d.put32 (base, 44, 2); d.put16 (base, 48, 1); d.put16 (base, 50, 6);
		d.put (base, 71, "SIDECAR    ", 11); d.put (base, 82, "FAT32   ", 8);
		d.put16 (base, 510, 0xAA55);
	}
	for (uint32_t fs : {kStart + 1, kStart + 7})
	{
		d.put32 (fs, 0, 0x41615252); d.put32 (fs, 484, 0x61417272);
		d.put32 (fs, 488, s.volClusters - 3); d.put32 (fs, 492, 4);	// free count and next free
		d.put32 (fs, 508, 0xAA550000);
	}
	for (uint32_t i = 0; i < 2; i++)
	{
		uint32_t fat = kStart + kRsv + i * fz;
		d.put32 (fat, 0, 0x0FFFFFF8); d.put32 (fat, 4, 0x0FFFFFFF); d.put32 (fat, 8, 0x0FFFFFFF); d.put32 (fat, 12, 0x0FFFFFFF);
	}
	uint32_t root = kStart + kRsv + 2 * fz;
	d.put (root, 0, "SIDECAR    ", 11); d.put8 (root, 11, 0x08);
	d.put (root + kSpc, 0, "DATA-MARKER", 11);			// cluster 3
}

inline uint64_t mbrCount (MemDisk &d) { return d.get32 (0, 446 + 12); }
inline uint64_t bootTot (MemDisk &d, uint32_t at = kStart) { return d.get32 (at, 32); }

// the most the grown volume may have: the FATs' reach, or the card's
inline uint64_t expect (const Spec &s)
{
	uint64_t ov = overhead (s);
	uint64_t byCard = (s.cardSectors - kStart - ov) / kSpc;
	uint64_t byFat = 128ull * fatsz (s.capClusters) - 2;
	return ov + (byCard < byFat ? byCard : byFat) * kSpc;
}

} // namespace fix
