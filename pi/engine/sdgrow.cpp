// pi/engine/sdgrow.cpp - the first boot's grow of the SD image (sdgrow.h). The write order keeps every step's
// state bootable: FSInfo's free count first, then the partition entry, the backup boot sector, the boot sector.
// A run that finds the partition larger than the volume is a cut one: it finishes the same target.
#include "sdgrow.h"
#include <string.h>

namespace sdgrow {

namespace {

const uint32_t kRsv = 32, kSpc = 64, kBackup = 6, kFsInfo = 1, kBackupFsInfo = 7;
const uint64_t kMaxClusters = 0x0FFFFFF5;	// FAT32's own limit
const char kLabel[11] = {'S', 'I', 'D', 'E', 'C', 'A', 'R', ' ', ' ', ' ', ' '};

uint32_t le16 (const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t le32 (const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
void put32 (uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }

bool zero16 (const uint8_t *p) { for (int i = 0; i < 16; i++) if (p[i]) return false; return true; }

Result done (Status s, uint64_t o, uint64_t n = 0, int step = 0) { Result r = {s, o, n ? n : o, step}; return r; }

// Change one sector: read, let the edit run, write, read back, compare.
// 0 on success, else Status (IoError or VerifyFailed).
struct Edit { int off; uint32_t value; };

int patch (Disk &rDisk, uint64_t lba, const Edit &e)
{
	alignas (16) uint8_t s[512], b[512];
	if (!rDisk.read (lba, s)) return IoError;
	put32 (s + e.off, e.value);
	if (!rDisk.write (lba, s)) return IoError;
	if (!rDisk.read (lba, b)) return IoError;
	return memcmp (s, b, 512) ? VerifyFailed : -1;
}

} // namespace

Result grow (Disk &rDisk)
{
	const uint64_t card = rDisk.sectors ();
	if (card == 0 || card == ~0ull) return done (NoRoom, 0);

	alignas (16) uint8_t mbr[512], bs[512], bk[512], fi[512], fb[512];	// Circle's EMMC reads and writes whole words
	if (!rDisk.read (0, mbr)) return done (IoError, 0);
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) return done (NoMbr, 0);
	const uint8_t *e0 = mbr + 446;
	if (!zero16 (e0 + 16) || !zero16 (e0 + 32) || !zero16 (e0 + 48)) return done (NotOurLayout, 0);
	const uint64_t start = le32 (e0 + 8), part = le32 (e0 + 12);
	if ((e0[4] != 0x0B && e0[4] != 0x0C) || start == 0 || part == 0) return done (NotOurLayout, part);

	if (!rDisk.read (start, bs) || !rDisk.read (start + kBackup, bk) || !rDisk.read (start + kFsInfo, fi)
	    || !rDisk.read (start + kBackupFsInfo, fb)) return done (IoError, part);
	const uint32_t fatsz = le32 (bs + 36);
	const uint64_t tot = le32 (bs + 32);
	const uint64_t overhead = kRsv + 2ull * fatsz;
	bool ok = bs[510] == 0x55 && bs[511] == 0xAA && le16 (bs + 11) == 512 && bs[13] == kSpc && le16 (bs + 14) == kRsv
		  && bs[16] == 2 && le16 (bs + 17) == 0 && le16 (bs + 19) == 0 && le16 (bs + 22) == 0 && fatsz > 0
		  && le32 (bs + 28) == start && le32 (bs + 44) == 2 && le16 (bs + 48) == kFsInfo && le16 (bs + 50) == kBackup
		  && memcmp (bs + 71, kLabel, 11) == 0 && tot > overhead;
	// the backup boot sector and both FSInfo sectors: valid, and the backup the same volume
	ok = ok && bk[510] == 0x55 && bk[511] == 0xAA && memcmp (bk + 71, kLabel, 11) == 0 && le32 (bk + 36) == fatsz;
	const uint8_t *infos[2] = {fi, fb};
	for (const uint8_t *f : infos)
		ok = ok && le32 (f) == 0x41615252 && le32 (f + 484) == 0x61417272 && le32 (f + 508) == 0xAA550000;
	if (!ok) return done (NotOurVolume, part);

	// the target: the card's end or the FATs' reach, in whole clusters
	if (card <= start + overhead) return done (NoRoom, part);
	uint64_t clusters = (card - start - overhead) / kSpc;
	const uint64_t reach = (uint64_t) fatsz * 128 - 2;
	if (clusters > reach) clusters = reach;
	if (clusters > kMaxClusters) clusters = kMaxClusters;
	if (clusters > (0xFFFFFFFFull - start - overhead) / kSpc) clusters = (0xFFFFFFFFull - start - overhead) / kSpc;	// the entry's 32 bits hold the end too
	const uint64_t target = overhead + clusters * kSpc;

	// the partition and the volume agree, or this is a cut run's leftover: the partition already at the target
	const bool resume = part != tot;
	if (resume && !(part > tot && part == target)) return done (NotOurVolume, part);
	if (start + part > card) return done (NoRoom, part);
	if (target <= tot) return done (Full, tot);
	if (!resume && target - tot < kMinGain) return done (SmallGain, tot);

	// the FATs must really be oversized: the entries of the clusters to be added are zero in both FATs. FAT 1 is read
	// whole over the range, in chunks (up to 64 MiB at the cap, once, at the first boot); FAT 2 is sampled: the first
	// and the last sector of the range and one in every 64 between.
	{
		const uint64_t firstE = (tot - overhead) / kSpc + 2, lastE = clusters + 1;
		const uint64_t s0 = firstE * 4 / 512, s1 = lastE * 4 / 512;
		const uint32_t firstOff = (uint32_t) (firstE * 4 % 512), lastEnd = (uint32_t) (lastE * 4 % 512) + 4;
		alignas (16) static uint8_t chunk[16 * 512];		// Circle's EMMC reads whole words; static: not on the 16 KB-ish stack
		for (uint64_t sec = s0; sec <= s1;)
		{
			uint32_t n = s1 - sec + 1 < 16 ? (uint32_t) (s1 - sec + 1) : 16;
			if (!rDisk.readSectors (start + kRsv + sec, chunk, n)) return done (IoError, part);
			for (uint32_t i = sec == s0 ? firstOff : 0; i < n * 512u; i++)
				if (chunk[i] && !(sec + i / 512 == s1 && i % 512 >= lastEnd)) return done (NotOurVolume, part);
			sec += n;
		}
		for (uint64_t sec = s0; sec <= s1; sec = sec == s1 ? s1 + 1 : (sec + 64 < s1 ? sec + 64 : s1))
		{
			alignas (16) uint8_t f[512];
			if (!rDisk.read (start + kRsv + fatsz + sec, f)) return done (IoError, part);
			for (uint32_t i = sec == s0 ? firstOff : 0; i < (sec == s1 ? lastEnd : 512u); i++)
				if (f[i]) return done (NotOurVolume, part);
		}
	}

	// the write order of the layout contract; the first mismatch stops it
	// FSInfo first: "unknown" is as valid for the old size as for the new, so a cut never leaves a stale count
	const struct { uint64_t lba; Edit e; } steps[5] = {
		{start + kFsInfo, {488, 0xFFFFFFFFu}},		// the free count: unknown, FatFs counts it; next free stays
		{start + kBackupFsInfo, {488, 0xFFFFFFFFu}},
		{0, {446 + 12, (uint32_t) target}},
		{start + kBackup, {32, (uint32_t) target}},
		{start, {32, (uint32_t) target}},
	};
	for (int i = 0; i < 5; i++)
	{
		int r = patch (rDisk, steps[i].lba, steps[i].e);
		if (r >= 0) return done ((Status) r, tot, 0, i + 1);
	}
	return done (Grown, tot, target);
}

const char *text (Status s)
{
	switch (s)
	{
	case Grown:		return "grown";
	case Full:		return "already fills the card";
	case SmallGain:		return "less than 64 MB to gain";
	case NoMbr:		return "no partition table";
	case NotOurLayout:	return "not the Sidecar image's partition layout";
	case NotOurVolume:	return "not the Sidecar image's volume";
	case NoRoom:		return "no room on the card";
	case IoError:		return "read or write failed";
	case VerifyFailed:	return "a sector read back wrong";
	}
	return "?";
}

} // namespace sdgrow
