// pi/engine/sdgrow.h - the first boot's "grow the card": the SD card image (about 2.5 GiB) stretches its one partition
// and its FAT32 volume to fill the card. Pure logic behind a sector interface (no Circle types); kernel.cpp wraps
// CEMMCDevice in a Disk. The disk layout and the write order are shared with the SD image builder.
#ifndef SDGROW_H
#define SDGROW_H

#include <stdint.h>

namespace sdgrow {

// 512-byte sectors, whole sectors only. read and write return false on a failure.
class Disk
{
public:
	virtual ~Disk () {}
	virtual uint64_t sectors () const = 0;		// the whole device
	virtual bool read (uint64_t lba, void *pSector) = 0;
	virtual bool write (uint64_t lba, const void *pSector) = 0;
	// n sectors in one go (the FAT check); the default is n single reads, the kernel's disk reads them in one command
	virtual bool readSectors (uint64_t lba, void *p, uint32_t n)
	{
		for (uint32_t i = 0; i < n; i++) if (!read (lba + i, (uint8_t *) p + 512 * (uint64_t) i)) return false;
		return true;
	}
};

enum Status
{
	Grown,		// the volume now fills the card (or the FATs' reach)
	Full,		// nothing to do: already as large as the card or the FATs allow
	SmallGain,	// less than 64 MiB to gain: left alone
	NoMbr,		// sector 0 has no MBR signature, or the read failed
	NotOurLayout,	// not exactly one FAT32 partition of the image's kind
	NotOurVolume,	// the boot sector is not the image's volume (label, geometry, sizes)
	NoRoom,		// the card is not larger than the partition, or its size is unknown
	IoError,	// a read or write failed
	VerifyFailed	// a sector read back differently from what was written
};

struct Result
{
	Status status;
	uint64_t oldSectors;	// the partition before
	uint64_t newSectors;	// after (Grown), else the same as old
	int failStep;		// IoError / VerifyFailed on a write: 1..5 (FSInfo, backup FSInfo, MBR, backup boot, boot); 0 = a read
};

const uint64_t kMinGain = 64ull * 1024 * 1024 / 512;	// sectors: the least a grow must add

// Whether the boot may grow at all: sdgrow=1 (the default) and not an engine test mode (testsecs=), which says the
// card is not the engine's to change.
inline bool allowed (int option, bool testMode) { return option != 0 && !testMode; }

Result grow (Disk &rDisk);
const char *text (Status s);	// a short reason for the log

} // namespace sdgrow

#endif
