// test_sdgrow.cpp - sdgrow.cpp (the first boot's grow of the SD image) on in-memory disks built by sdimg_fixture.h:
// a fresh image grows (and only the four metadata sectors change), a second run does nothing, every refusal leaves
// the disk untouched, the FATs' reach caps the size, a power cut after each write is finished by the next boot.
#include "sdgrow.h"
#include "sdimg_fixture.h"
#include "check.h"
#include <stdio.h>

using namespace fix;
using sdgrow::Status;
using sdgrow::Grown;
using sdgrow::kMinGain;

static std::map<uint64_t, MemDisk::Sec> snap (MemDisk &d) { return d.m; }

// the sectors whose bytes differ between two disks
static std::vector<uint64_t> diff (const std::map<uint64_t, MemDisk::Sec> &a, const std::map<uint64_t, MemDisk::Sec> &b)
{
	std::vector<uint64_t> r;
	for (auto &kv : a) { auto it = b.find (kv.first); if (it == b.end () || it->second != kv.second) r.push_back (kv.first); }
	for (auto &kv : b) if (!a.count (kv.first)) r.push_back (kv.first);
	return r;
}

// a run that must refuse: the status, no write, no change
static void refused_at (int line, MemDisk &d, Status want)
{
	auto before = snap (d);
	sdgrow::Result r = sdgrow::grow (d);
	if (r.status != want) fprintf (stderr, "line %d: got \"%s\", wanted \"%s\"\n", line, sdgrow::text (r.status), sdgrow::text (want));
	CHECK (r.status == want);
	CHECK (d.writes == 0);
	CHECK (diff (before, d.m).empty ());
}

#define refused(d, want) refused_at (__LINE__, d, want)

static void test_grows_fresh ()
{
	Spec s; MemDisk d; build (d, s);
	auto before = snap (d);
	uint64_t oldTot = volSectors (s);
	sdgrow::Result r = sdgrow::grow (d);
	CHECK (r.status == sdgrow::Grown);
	CHECK (r.oldSectors == oldTot);
	uint64_t want = expect (s);
	CHECK (r.newSectors == want);
	CHECK (want > oldTot && kStart + want <= s.cardSectors);
	CHECK ((want - overhead (s)) % kSpc == 0);			// whole clusters
	CHECK (mbrCount (d) == want);
	CHECK (bootTot (d) == want && bootTot (d, kStart + 6) == want);
	for (uint32_t fs : {kStart + 1, kStart + 7}) { CHECK (d.get32 (fs, 488) == 0xFFFFFFFFu); CHECK (d.get32 (fs, 492) == 4); CHECK (d.get32 (fs, 0) == 0x41615252); }
	// nothing else changed: not the FATs, the root, the data, the label, the entries 1..3, the signature
	std::vector<uint64_t> ch = diff (before, d.m);
	std::vector<uint64_t> exp = {0, kStart, kStart + 1, kStart + 6, kStart + 7};
	CHECK (ch == exp);
	CHECK (d.writes == 5);
	// the bytes of those sectors that are not the size or the free count are as before
	MemDisk o; build (o, s);
	uint8_t a[512], b[512];
	for (uint64_t l : exp) { d.read (l, a); o.read (l, b); int n = 0; for (int i = 0; i < 512; i++) n += a[i] != b[i]; CHECK (n > 0 && n <= 4); }
}

static void test_idempotent ()
{
	Spec s; MemDisk d; build (d, s);
	CHECK (sdgrow::grow (d).status == Grown);
	auto before = snap (d); d.writes = 0;
	sdgrow::Result r = sdgrow::grow (d);
	CHECK (r.status == sdgrow::Full);
	CHECK (d.writes == 0 && diff (before, d.m).empty ());
}

static void test_refusals ()
{
	{ Spec s; MemDisk d; build (d, s); d.put16 (0, 510, 0); refused (d, sdgrow::NoMbr); }
	{ Spec s; MemDisk d; build (d, s); d.put8 (0, 446 + 16 + 4, 0x83); d.put32 (0, 446 + 16 + 8, 99999999); d.put32 (0, 446 + 16 + 12, 100); refused (d, sdgrow::NotOurLayout); }	// two partitions
	{ Spec s; MemDisk d; build (d, s); d.put8 (0, 446 + 48 + 1, 1); refused (d, sdgrow::NotOurLayout); }		// entry 3 not zero
	{ Spec s; MemDisk d; build (d, s); d.put8 (0, 446 + 4, 0x83); refused (d, sdgrow::NotOurLayout); }		// Linux type
	{ Spec s; MemDisk d; build (d, s); d.put8 (0, 446 + 4, 0x0B); CHECK (sdgrow::grow (d).status == sdgrow::Grown); }	// 0x0B is fine
	{ Spec s; MemDisk d; build (d, s); d.put32 (0, 446 + 8, 0); refused (d, sdgrow::NotOurLayout); }		// no start
	{ Spec s; MemDisk d; build (d, s); d.put16 (kStart, 510, 0); refused (d, sdgrow::NotOurVolume); }		// boot sector signature
	{ Spec s; MemDisk d; build (d, s); d.put (kStart, 71, "MYCARD     ", 11); refused (d, sdgrow::NotOurVolume); }	// an OS-formatted card: another label
	{ Spec s; MemDisk d; build (d, s); d.put (kStart, 71, "sidecar    ", 11); refused (d, sdgrow::NotOurVolume); }	// the label is exact
	{ Spec s; MemDisk d; build (d, s); d.put16 (kStart, 11, 4096); refused (d, sdgrow::NotOurVolume); }		// 4 KiB sectors
	{ Spec s; MemDisk d; build (d, s); d.put16 (kStart, 22, 100); refused (d, sdgrow::NotOurVolume); }		// FAT16 field
	{ Spec s; MemDisk d; build (d, s); d.put32 (kStart, 36, 0); refused (d, sdgrow::NotOurVolume); }		// no FAT32 size
	{ Spec s; MemDisk d; build (d, s); d.put8 (kStart, 13, 8); refused (d, sdgrow::NotOurVolume); }		// 4 KiB clusters
	{ Spec s; MemDisk d; build (d, s); d.put8 (kStart, 16, 1); refused (d, sdgrow::NotOurVolume); }		// one FAT
	{ Spec s; MemDisk d; build (d, s); d.put16 (kStart, 14, 4); refused (d, sdgrow::NotOurVolume); }		// 4 reserved sectors
	{ Spec s; MemDisk d; build (d, s); d.put16 (kStart, 17, 512); refused (d, sdgrow::NotOurVolume); }		// a FAT16 root dir
	{ Spec s; MemDisk d; build (d, s); d.put32 (kStart, 28, 63); refused (d, sdgrow::NotOurVolume); }		// hidden sectors != start
	{ Spec s; MemDisk d; build (d, s); d.put16 (kStart + 6, 510, 0); refused (d, sdgrow::NotOurVolume); }		// no backup boot sector
	{ Spec s; MemDisk d; build (d, s); d.put32 (kStart + 1, 484, 0); refused (d, sdgrow::NotOurVolume); }		// FSInfo signature
	{ Spec s; MemDisk d; build (d, s); d.put32 (kStart + 7, 508, 0); refused (d, sdgrow::NotOurVolume); }		// backup FSInfo signature
	{ Spec s; MemDisk d; build (d, s); d.put32 (0, 446 + 12, (uint32_t) volSectors (s) - 64); refused (d, sdgrow::NotOurVolume); }	// partition != TotSec32 (smaller)
	{ Spec s; MemDisk d; build (d, s); d.put32 (0, 446 + 12, (uint32_t) volSectors (s) + 64); refused (d, sdgrow::NotOurVolume); }	// larger, not what the card gives
	{ Spec s; MemDisk d; build (d, s); d.put32 (kStart, 32, (uint32_t) volSectors (s) + 64); refused (d, sdgrow::NotOurVolume); }
}

static void test_no_room_and_small_gain ()
{
	{ Spec s; s.cardSectors = kStart + volSectors (s); MemDisk d; build (d, s); refused (d, sdgrow::Full); }	// the card ends with the partition: nothing to add
	{ Spec s; s.cardSectors = kStart + volSectors (s) - 1; MemDisk d; build (d, s); refused (d, sdgrow::NoRoom); }	// smaller than the partition
	{ Spec s; MemDisk d; build (d, s); d.total = ~0ull; refused (d, sdgrow::NoRoom); }				// size unknown
	{ Spec s; MemDisk d; build (d, s); d.total = 0; refused (d, sdgrow::NoRoom); }
	{ Spec s; s.cardSectors = kStart + volSectors (s) + kMinGain - kSpc; MemDisk d; build (d, s); refused (d, sdgrow::SmallGain); }	// just under 64 MiB
	{ Spec s; s.cardSectors = kStart + volSectors (s) + kMinGain + 2 * kSpc; MemDisk d; build (d, s);			// just over
	  sdgrow::Result r = sdgrow::grow (d); CHECK (r.status == sdgrow::Grown); CHECK (r.newSectors - r.oldSectors >= kMinGain); }
	// the FATs reach almost no further than the volume: a "FATs too small" card is left alone
	{ Spec s; s.capClusters = s.volClusters + 100; MemDisk d; build (d, s); refused (d, sdgrow::SmallGain); }
	{ Spec s; s.capClusters = s.volClusters - 200; MemDisk d; build (d, s); refused (d, sdgrow::Full); }
}

static void test_cap ()
{
	// the card is far beyond what the FATs address: the volume stops at the FATs' reach, in whole clusters
	{ Spec s; s.cardSectors = 1ull << 33; MemDisk d; build (d, s);
	  sdgrow::Result r = sdgrow::grow (d);
	  CHECK (r.status == sdgrow::Grown);
	  CHECK (r.newSectors == expect (s));
	  CHECK (r.newSectors == overhead (s) + (128ull * fatsz (s.capClusters) - 2) * kSpc);
	  CHECK (sdgrow::grow (d).status == sdgrow::Full); }
	// the real contract: FATs for 512 GiB, a 4 TiB card (sparse disk) -> about 512 GiB
	{ Spec s; s.cardSectors = 8ull << 30; s.capClusters = 16773100; s.volClusters = 70000;
	  MemDisk d; build (d, s);
	  sdgrow::Result r = sdgrow::grow (d);
	  CHECK (r.status == sdgrow::Grown);
	  CHECK (r.newSectors * 512 <= (512ull << 30) + (1 << 20) && r.newSectors * 512 > (512ull << 30) - (1 << 20)); }
	// a card one cluster short of the FATs' reach: the card caps
	{ Spec s; uint64_t reach = overhead (s) + (128ull * fatsz (s.capClusters) - 2) * kSpc;
	  s.cardSectors = kStart + reach - 1; MemDisk d; build (d, s);
	  sdgrow::Result r = sdgrow::grow (d); CHECK (r.status == sdgrow::Grown); CHECK (r.newSectors == reach - kSpc); CHECK (kStart + r.newSectors <= s.cardSectors); }
}

// Torn sector writes are not modelled: every edit is a 4-byte field inside one sector, so a half-written sector
// is either the old or the new value of that field.
static void test_power_cut ()
{
	Spec s; MemDisk clean; build (clean, s); sdgrow::grow (clean);
	for (int cut = 0; cut < 5; cut++)
	{
		MemDisk d; build (d, s);
		d.cutAfter = cut;
		sdgrow::Result r = sdgrow::grow (d);
		CHECK (r.status == sdgrow::IoError && r.failStep == cut + 1);
		// the half-grown card is one the next boot understands: the volume never larger than the partition
		CHECK (mbrCount (d) >= bootTot (d));
		d.cutAfter = -1; d.writes = 0;
		r = sdgrow::grow (d);
		// every cut ends right: all sizes, and FSInfo unknown
		CHECK (r.status == sdgrow::Grown);
		CHECK (diff (clean.m, d.m).empty ());
		CHECK (d.get32 (kStart + 1, 488) == 0xFFFFFFFFu && d.get32 (kStart + 7, 488) == 0xFFFFFFFFu);
	}
	// a cut after the MBR, twice in a row
	{ MemDisk d; build (d, s); d.cutAfter = 3; sdgrow::grow (d); d.cutAfter = 4; d.writes = 0; sdgrow::Result r = sdgrow::grow (d); CHECK (r.status == sdgrow::IoError);
	  d.cutAfter = -1; CHECK (sdgrow::grow (d).status == sdgrow::Grown); CHECK (diff (clean.m, d.m).empty ()); }
	// a resumed card moved to a card of another size is refused, not guessed at
	{ MemDisk d; build (d, s); d.cutAfter = 3; sdgrow::grow (d); d.cutAfter = -1; d.writes = 0; d.total = kStart + volSectors (s) + 200000; refused (d, sdgrow::NotOurVolume); }
}

static void test_verify_and_io ()
{
	{ Spec s; MemDisk d; build (d, s); d.lie = true; sdgrow::Result r = sdgrow::grow (d); CHECK (r.status == sdgrow::VerifyFailed && r.failStep == 1); CHECK (d.writes == 1); }	// stops at the first mismatch
	{ Spec s; MemDisk d; build (d, s); d.cutAfter = 0; sdgrow::Result r = sdgrow::grow (d); CHECK (r.status == sdgrow::IoError && r.failStep == 1); }
	{ MemDisk d; d.total = 1 << 20; sdgrow::Result r = sdgrow::grow (d); CHECK (r.status == sdgrow::NoMbr); CHECK (d.writes == 0); }	// an empty card
}

// M2: the FAT entries of the clusters to be added must be zero (both FATs), or the card is not the image's
static void test_fat_new_range ()
{
	Spec s; s.cardSectors = 1ull << 22; s.volClusters = 2000; s.capClusters = 100000;	// 2 GiB card: the new range spans about 500 FAT sectors
	uint32_t fz = fatsz (s.capClusters);
	uint32_t fat1 = kStart + kRsv, fat2 = kStart + kRsv + fz;
	uint32_t firstE = s.volClusters + 2, firstSec = firstE * 4 / 512;
	// a non-zero entry in the new range: first entry, last sector, a sampled sector in between; either FAT
	struct { uint32_t fat, sec, off; } hits[] = {
		{fat1, firstSec, (firstE * 4) % 512}, {fat2, firstSec, (firstE * 4) % 512},
		{fat1, firstSec + 64, 100}, {fat2, firstSec + 64, 100},	// the sampled ones: every 64th {fat1, firstSec, 508 /* the sector's last entry */},
	};
	for (auto &h : hits)
	{
		MemDisk d; build (d, s);
		d.put32 (h.fat + h.sec, h.off, 0x00000005);
		refused (d, sdgrow::NotOurVolume);
	}
	// FAT 1 is read whole: a stray entry in a sector the sampling (first, last, every 64th) misses is found,
	// also across a chunk's edge (16 sectors) and in the sector before the last
	{
		uint64_t tgt = expect (s); uint64_t lastE = (tgt - overhead (s)) / kSpc + 1, lastSec = lastE * 4 / 512;
		for (uint32_t sec : {firstSec + 1, firstSec + 7, firstSec + 15, firstSec + 16, firstSec + 17, firstSec + 100, (uint32_t) lastSec - 1})
		{
			MemDisk d; build (d, s);
			d.put8 (fat1 + sec, 300, 1);
			refused (d, sdgrow::NotOurVolume);
		}
		MemDisk d; build (d, s); d.put8 (fat2 + firstSec + 7, 300, 1);	// FAT 2 stays sampled: documented, not found
		CHECK (sdgrow::grow (d).status == Grown);
	}
	// the last sector of the new range, in either FAT
	for (uint32_t fat : {fat1, fat2})
	{
		MemDisk d; build (d, s);
		uint64_t tgt = expect (s); uint64_t lastE = (tgt - overhead (s)) / kSpc + 1;
		d.put32 (fat + lastE * 4 / 512, (lastE * 4) % 512, 0x0FFFFFFF);
		refused (d, sdgrow::NotOurVolume);
	}
	// entries of the old clusters (before the new range, the same sector) may be anything
	{ MemDisk d; build (d, s); d.put32 (fat1 + firstSec, (firstE * 4) % 512 - 4, 0x0FFFFFFF); CHECK (sdgrow::grow (d).status == Grown); }
	// a cut run's second pass has the same zero FATs and finishes
	{ MemDisk d; build (d, s); d.cutAfter = 3; sdgrow::grow (d); d.cutAfter = -1; CHECK (sdgrow::grow (d).status == Grown); }
}

// L2: the start LBA counts in the 32-bit limit of the partition entry
static void test_u32_limit ()
{
	Spec s; s.cardSectors = 1ull << 34; s.capClusters = 268000000; s.volClusters = 2000;
	MemDisk d; build (d, s);
	sdgrow::Result r = sdgrow::grow (d);
	CHECK (r.status == Grown);
	CHECK (kStart + r.newSectors <= 0xFFFFFFFFull);
	CHECK (mbrCount (d) == r.newSectors);
}

// M1: the engine's test modes (testsecs=) and sdgrow=0 never grow
static void test_allowed ()
{
	CHECK (sdgrow::allowed (1, false));
	CHECK (!sdgrow::allowed (0, false));
	CHECK (!sdgrow::allowed (1, true));
	CHECK (!sdgrow::allowed (0, true));
}

int main ()
{
	test_grows_fresh ();
	test_idempotent ();
	test_refusals ();
	test_no_room_and_small_gain ();
	test_cap ();
	test_power_cut ();
	test_verify_and_io ();
	test_fat_new_range ();
	test_u32_limit ();
	test_allowed ();
	for (int s = 0; s <= sdgrow::VerifyFailed; s++) CHECK (sdgrow::text ((Status) s)[0] != 0);
	printf ("test_sdgrow: ok\n");
	return 0;
}
