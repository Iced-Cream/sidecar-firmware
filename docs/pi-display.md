# Pi display

A display attached to the Pi is a third screen target for the boxes' drawing service (port plan step S8k, decided
2026-11-28). It shows the same 128x64 pages a client draws for a box's screen, scaled up, with one Pi-wide stack that
clients of both boxes push onto. There are no LVGL pages any more (the old `pi/ui` tree went with the strip-down in
4c4c805); the driver below is a minimal one taken from it (3a8bf7d).

**State:** host-tested only. No unit run has used it and no panel was at hand, so the display is **off unless cmdline
`pidisplay=1`**. QEMU has no display either.

## What it shows

- **Top of the stack:** the top client's frame (`core/screen`'s `Client::draw`), text items drawn with the CC0 small font
  (`ui::preview`), scaled by the largest whole number that leaves room for the soft-key bar: 6 on 800x480, 8 on
  1024x600, 10 on 1280x720. Only the 8x8 tiles that changed go to the panel.
- **Empty stack:** an idle frame (SIDECAR, NO PAGE OPEN, the network's address, the engine version).
- **Soft-key bar:** CLOSE LEFT UP DOWN RIGHT NO YES. A touch on a key sends that key, down and up, to the top client
  only (Rytm codes 0x40..0x45 until S8h's per-box key tables; `pidisplay::set_keymap`). CLOSE closes the top entry. A
  touch on the page itself does nothing. No encoders.
- **Pi-wide popups:** the pairing code (`bridge::pair_popup`) draws over whatever is on top, as on the boxes' screens. Any
  touch ends it and goes no further.

## The stack

The rules are the box stacks' (`Projects\Sidecar\notes\design-s8h-screen-stack-status-2026-11-28.md` section 2), held on
the Pi because no box owns this screen (`core/pidisplay/pidisplay.h`):

- Every request goes on top; a client already on the stack moves to the top. At most 8 entries.
- A covered entry keeps its state and gets no draw or key calls.
- Close removes an entry wherever it is. Closing the top shows the one below again with a whole frame (`opened (true)`).
- Any client may open itself (S8h Q3); opening from the box is recommended.
- An entry is (slot, client): the same client for the other box is another entry.

What opens a page on the display (a box mod's request over the host link, a Pi client's own request) is not wired yet:
it comes with S8h's client registry and ops (18-1C / 58-5C). Until then `pidisplay::push` / `close` are the API.

## Driver (`pi/engine/pi_display.cpp`)

- **Frame buffer:** the firmware's, 16 bits, on the DSI display when the firmware reports one (`dsi=0`: display 0), else
  HDMI. The control core writes rows straight into it with a cache clean per row.
- **Touch:** a USB touch screen (Circle's HID digitizer driver with `circle-touch*.patch`) has priority over the DSI
  display's firmware touch buffer (the official 7" display; `dsitouch_inv=1|2|3` flips it). Bound at plug-and-play.
- **Calibration:** `touchcal=x0,x1,y0,y1,swap` (`core/pidisplay/touch_cal.h`); without it the USB descriptor's logical
  range, else 12/15-bit auto. There is no calibration page.
- **Left out** from the old driver: LVGL, the boot splash, themes, EDID, pinch, the screensaver, mouse, and the Pi 5
  (`addon/rp1dsi`, portrait panels; their patches are not on this line).

## Tests

`make -C core test`: `test_pidisplay` (layouts, pixels, the stack rules, keys, popups; screenshots in
`core/build/pidisplay_*.ppm`) and `test_touch_cal`.

## Gaps

- No hardware: neither panel nor touch has run on this line. The Circle side compiles and links into the kernel.
- No Pi 5 display.
- A4 key codes: the A4's clients get Rytm codes until S8h's key tables exist.
