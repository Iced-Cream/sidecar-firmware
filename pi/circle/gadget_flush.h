// pi/circle/gadget_flush.h - a bit of a DWC2 register that clears by itself in some PHY clocks, polled: no timer, no log, a bounded number of reads.
// For interrupt level: CDWUSBGadget::WaitForBit waits in 1 ms steps (MsDelay) and logs its timeout, which in HandleUSBReset held core 0's other
// interrupts, so the Rytm's isochronous blocks with them (8 URBs cover 1.17 ms; the endpoint's own flush was changed for this before,
// circle-gadget.patch). Copied into the tree as circle/usb/gadget/gadgetflush.h (pi/engine/build.sh); replayed on the Mac by
// pi/engine/tests/test_gadget_flush.cpp. false: the reads ran out, the bit still set (the caller goes on: a reset does not wait for a stuck core).
#ifndef _circle_usb_gadget_gadgetflush_h
#define _circle_usb_gadget_gadgetflush_h

static const unsigned kGadgetFlushReads = 5000;

template <class TRegister>
inline bool GadgetBitClears (TRegister &rReg, unsigned nMask)
{
	for (unsigned i = 0; i < kGadgetFlushReads; i++)
	{
		if (!(rReg.Read () & nMask))
		{
			return true;
		}
	}
	return false;
}

#endif
