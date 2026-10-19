// pi/engine/sdfiles.h - the SD card's boot files over TCP (port 7310; cmdline sdport=0 turns it off), so a build is
// installed to the card, and config.txt/cmdline.txt are read and edited, without taking the card out
// (docs/pi-engine.md, "Boot from the SD card"; the Mac's side: pi/circle/sdboot.py). One connection at a time,
// line commands, every reply one line ("OK ..." or "ERR ..."):
//   LS                       OK <n>, then n lines "<size> <name>" (the card's root)
//   GET <name>               OK <size> <crc32>, then the bytes
//   PUT <name> <size> <crc32>, then the bytes: written to <name>.new, read back and checked, then put in place.
//                            An existing <name> is kept as <name>.prev (the last one); config.txt and cmdline.txt
//                            keep their first version as <name>.bak (the original, never replaced). A failed
//                            check leaves <name> as it was.
//   SCRATCH                  writes, reads back and deletes sdtest.tmp (64 KB): the write path's test
//   DEL <name>               only engine.img*, sdboot*, tryboot.txt*, *.tmp and *.new (tryboot.txt is never
//                            kept on a PUT)
//   REBOOT | TRYBOOT         the engine stops and reboots; TRYBOOT sets the firmware's one-shot tryboot flag
//                            first (the next boot reads tryboot.txt instead of config.txt; a power cycle does not)
// Names: the root only, letters, digits, '.', '-', '_'. Runs as a task on core 0 (FatFs is reentrant).
#ifndef _pi_engine_sdfiles_h
#define _pi_engine_sdfiles_h

#include <circle/netdevice.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/task.h>
#include <circle/serial.h>
#include <circle/types.h>

class CSdFiles : public CTask
{
public:
	static const u16 Port = 7310;
	enum TReboot { RebootNone, RebootPlain, RebootTry };
	CSdFiles (CNetSubSystem *pNet, CSerialDevice *pSerial = 0);	// pSerial: the serial port's instance
	static void RequestSerial (void);		// the console's ":sdf": the serial port is this task's until BYE
	static boolean SerialActive (void)	{ return s_bSerial; }
	void Run (void) override;
	static TReboot TakeReboot (void);		// core 0's loop: a REBOOT/TRYBOOT asked for
	static boolean SetTryboot (void);		// the firmware's tryboot flag (mailbox 0x00038064)
private:
	void Serve (void);
	boolean Fill (void);
	void SendRaw (const void *pData, unsigned nLen);
	int RecvByte (void);
	boolean ReadLine (char *pLine, unsigned nSize);
	boolean RecvExact (u8 *pBuf, unsigned nLen);
	void Reply (const char *pFormat, ...);
	void List (void);
	void Get (const char *pName);
	void Put (const char *pName, unsigned nSize, u32 nCRC);
	void Scratch (void);
	CNetSubSystem *m_pNet;
	CSerialDevice *m_pSerial;
	CSocket *m_pConn;
	u8 *m_pBuf;					// 16 KB
	u8 m_Rx[FRAME_BUFFER_SIZE];			// Receive's buffer (at least a frame: Circle drops the rest)
	unsigned m_nRxPos, m_nRxLen;
	char m_Line[1500];				// serial: a "D <base64>" line (up to 1024 bytes)
	u8 m_Dec[1100];
	unsigned m_nDecPos, m_nDecLen;
	static volatile int s_nReboot;
	static volatile boolean s_bSerial;
};

#endif
