// pi/engine/usbmidi_pack.h - one whole SysEx (F0 .. F7) as USB MIDI 1.0 event packets on cable 0, for the Rytm's
// USB MIDI OUT endpoint (ob_usb.cpp's CObUsbTransport::Poll): CIN 4 for each 3 bytes that do not end it, then CIN
// 5, 6 or 7 for the last 1, 2 or 3 bytes (F7 among them), zero padded. out must hold usbmidi_sysex_bytes(n) bytes.
// Returns the packets' bytes, or 0 when m is not one whole SysEx (the core only ever sends whole ones).
// Portable (Mac-tested: tests/test_usbmidi_pack.cpp).
#pragma once
#include <stdint.h>

inline int usbmidi_sysex_bytes(int n) { return 4 * ((n + 2) / 3); }

inline int usbmidi_pack_sysex(const uint8_t* m, int n, uint8_t* out) {
  if (n < 2 || m[0] != 0xF0 || m[n - 1] != 0xF7) return 0;
  for (int i = 1; i < n - 1; ++i)
    if (m[i] & 0x80) return 0;   // a status byte inside: not one SysEx
  int o = 0;
  for (int i = 0; i < n; i += 3) {
    int k = n - i < 3 ? n - i : 3;
    out[o] = (uint8_t)(i + k == n ? 4 + k : 4);
    for (int j = 0; j < 3; ++j) out[o + 1 + j] = j < k ? m[i + j] : 0;
    o += 4;
  }
  return o;
}

// One record of the core's for the Rytm: a whole SysEx as above, or a 3-byte channel message (the LFOs' CCs,
// core/lfo.h: CIN = its status's high nibble) as one packet. Returns the packets' bytes, 0 for anything else.
inline int usbmidi_pack(const uint8_t* m, int n, uint8_t* out) {
  if (n == 3 && m[0] >= 0x80 && m[0] < 0xF0 && !(m[1] & 0x80) && !(m[2] & 0x80)) {
    out[0] = (uint8_t)(m[0] >> 4), out[1] = m[0], out[2] = m[1], out[3] = m[2];
    return 4;
  }
  return usbmidi_pack_sysex(m, n, out);
}
