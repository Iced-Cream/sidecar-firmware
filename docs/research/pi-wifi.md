# Wi-Fi on the Pi 4 engine (BCM43455, Circle 51.1.1)

Research and a proof of concept (branch `pi-wifi`, `pi/engine/wifi.{h,cpp}`). Wi-Fi stays off by default
(the user's decision). Tags: **measured** = seen in this tree or on the Pi; **inferred** = read from code or docs, not
yet seen on hardware.

## What Circle has

- `addon/wlan`: the Plan 9 `ether4330` driver (SDIO, BCM4343x/43455/43456), wrapped as `CBcm4343Device`, a
  `CNetDevice` of type `NetDeviceTypeWLAN`. Plan 9 licence (`addon/wlan/Plan9License`, GPL-compatible), glue GPLv3.
- `addon/wlan/hostap`: a git submodule (rsta2/hostap at `966cab4`) with wpa_supplicant ported to Circle
  (`Makefile.circle`, `CWPASupplicant`, `driver_circle.cpp`, `l2_packet_circle.cpp`). BSD licence (hostap's COPYING:
  BSD only since 2012). It was not checked out in our tree. `pi/engine/build.sh` now clones it on the host, once.
- `sample/hello_wlan`: EMMC + FatFs mount, `CBcm4343Device ("SD:/firmware/")`, `CNetSubSystem (0,0,0,0, host,
  NetDeviceTypeWLAN)` (DHCP), `CWPASupplicant ("SD:/wpa_supplicant.conf")`. That is the whole bring-up.

## 1. Bring-up

**Firmware (inferred, from `addon/wlan/firmware/Makefile`):** a Pi 4 needs three files in `SD:/firmware/`, renamed
from RPi-Distro/firmware-nonfree at `c9d3ae65` (2025-04-08), `debian/config/brcm80211/`:

| on the card | source | size | licence |
|---|---|---|---|
| `brcmfmac43455-sdio.bin` | `cypress/cyfmac43455-sdio-minimal.bin` | 548728 B | Cypress binary-redist EULA |
| `brcmfmac43455-sdio.clm_blob` | `cypress/cyfmac43455-sdio.clm_blob` | 2676 B | Cypress binary-redist EULA |
| `brcmfmac43455-sdio.txt` (NVRAM) | `brcm/brcmfmac43455-sdio.txt` | 2074 B | GPL-2 (Linaro, Raspberry Pi) |

**Controllers (inferred from the code):** on the Pi 4 the SD card is on **EMMC2** (`ARM_EMMC2_BASE`, the `SDCard` addon,
`m_EmmcBase (... ARM_EMMC2_BASE)`, polled, no IRQ). The Wi-Fi chip is on the **legacy Arasan EMMC** (`0x7e300000`,
GPIO 34-39 ALT3, IRQ `ARM_IRQ_ARASANSDIO` = GIC SPI 126) through `addon/wlan/emmc.c`, Circle's own copy. They are
separate controllers, so our SD access does not clash. FatFs is built reentrant (`FF_FS_REENTRANT 1`), and the
firmware is read through it at bring-up.

**Scheduler:** the driver runs as Circle tasks (`kproc`: `wifireader`, `wifitimer`; wpa_supplicant's eloop is a third
task). Plan 9's `sleep` and `tsleep` are `CScheduler::Yield ()` loops, so the reader task polls instead of
blocking. Core 0 already runs Circle's scheduler for the network (`kernel.cpp`'s loop sleeps `MsSleep (1)`), so it
fits, but core 0 never really idles while Wi-Fi is up. **Interrupts off:** Plan 9's `lock ()` is
`EnterCritical (IRQ_LEVEL)` on core 0, and the Overbridge stream is handled in core 0's xHCI interrupt. Short
sections are fine; long ones would delay that stream (a risk to measure, see below).

**Size (measured):** `_end` 0x7c0018 → **0x829308** (+431 KB; 0x82ee48 on 4dc305f). wpa_supplicant ≈ 404 KB text, libwlan ≈ 42 KB. About
**348 KB** is left below 0x880000 (8 MB). If more room is needed: `--kernel-max-size 16` in the tree's configure,
which is shared and must be rebuilt.

**CPU on core 0:** to be measured (see the test steps). The PBKDF2 of a passphrase (4096 × SHA-1) runs once, at
`wifi=join` or at a boot with a plain passphrase in wifi.txt.

## 2. Scan, join, addressing, mDNS

- **Scan:** `Control ("escan N")`, then `ReceiveScanResult`. The PoC parses the escan BSS records as `driver_circle`
  does (SSID, RSSI, channel), keeps the strongest per SSID (24 at most), logs only SSIDs, and puts them in
  `/status`. While wpa_supplicant runs it owns the scan queue, so a joined Pi does not scan in the PoC.
- **WPA2-PSK:** wpa_supplicant with a generated `SD:/.rytm/wpa.conf`: the SSID in hex, `psk=` as the 64-hex PMK
  (PBKDF2-SHA1 from hostap's `sha1-pbkdf2.c`), `key_mgmt=WPA-PSK`, `scan_ssid=1`, `country=`. The passphrase itself is
  never written by `wifi=join`. It is never logged: web `/cmd` logs `wifi=join:...`, and an over-long serial line
  is redacted too. WPA3/SAE: not in Circle's build (no SAE objects in `Makefile.circle`) (inferred).
- **Addressing:** DHCP by default on Wi-Fi (Circle's `CDHCPClient`, hostname `sidecar`), static with wifi.txt
  `ip= netmask= gw= dns=` (all four). Ethernet keeps its static link-local default.
- **mDNS:** the same `CWebStart` (it waits for `IsRunning`, which is the DHCP lease on Wi-Fi), then Circle's mDNS
  with our patch. `CBcm4343Device::SetMulticastFilter` is implemented, so 224.0.0.251 is received (inferred).

## 3. Coexistence

- **Ethernet + Wi-Fi as two networks: no, without patching Circle.** `CNetSubSystem` is a singleton (`s_pThis`,
  asserted), its `CNetDeviceLayer` binds one `CNetDevice` by type, and wpa_supplicant's `l2_packet_circle` sends
  EAPOL through `CNetSubSystem::Get ()->GetLinkLayer ()`. `CmDNSDaemon` and the HTTP daemon also use the one stack.
  So the network is **either/or**, chosen at boot. Two stacks would mean a routing layer in lib/net (several
  interfaces, ARP and a route per interface, mDNS on both). That is a large patch.
- **The chip beside Ethernet: yes.** `CNetDevice` keeps up to 5 devices, and the stack picks Ethernet by type, so
  `wifi=scan` runs the chip for scans while Ethernet stays the network (the first-run setup case).
- **Audio:** cores 1-3 have no part in it. The Wi-Fi IRQ (SPI 126) goes to core 0, like every Circle IRQ. The risks
  are core 0's IRQ-off sections and its busy polling, against the xHCI stream's interrupt (to be measured).

## 4. Licences

- Circle: GPLv3. The engine is linked with it already, so nothing changes.
- addon/wlan driver: Plan 9 licence (GPL-compatible, per Circle).
- hostap/wpa_supplicant: BSD (3-clause, no advertising clause). Keep its copyright notice in our docs or package.
- Firmware `.bin` and `.clm_blob`: the Cypress EULA allows reproducing and distributing in object code form
  "solely for use in connection with Cypress integrated circuit products". The Pi 4's chip is a Cypress
  CYW43455, so shipping them in our SD package is allowed. Ship the licence text with them (`debian/copyright`, section
  "binary-redist-Cypress", in firmware-nonfree) and do not modify them. Raspberry Pi OS ships the same files.
- NVRAM `.txt`: GPL-2 (Linaro and Raspberry Pi copyright): ship it with its notice.
- None of these go into git here: the SD package's build fetches them (as Circle's `firmware/Makefile` does).

## 5. Effort and risks

The PoC (boot modes, wifi.txt, console, /status, DHCP + mDNS) is done (it builds). Its hardware test is pending.
Still to do for the product:

- the setup pages: the web page's network list and password field, then the display and the Rytm (~1-2 days);
- switching without a reboot: impossible with one stack, so "join" means "store, then reboot". A clean reboot into
  Wi-Fi (`StopAndReboot`) costs ~0.5 day;
- scanning while joined (read wpa_supplicant's BSS table) ~0.5 day;
- the firmware in the SD package and its licence notice ~0.5 day;
- a dual stack (Ethernet and Wi-Fi at once): several days of lib/net work. Not recommended.

Risks: core-0 IRQ latency against the Overbridge stream (Plan 9 `lock` = IRQs off), and core 0 never idling (heat:
measure the temperature); the 348 KB left in the kernel; DHCP on networks that are slow to answer (sidecar.local only
comes after the lease); the 2.4/5 GHz regulatory domain (wifi.txt `country=`, default NL); wpa_supplicant's
robustness on reconnects (Circle's port, little used); no WPA3.

## Hardware results (measured 2026-10-17, Pi 4, RAM kernel on 4dc305f, Rytm off)

- wifi=0: boots as before. Ethernet 169.254.61.60, sidecar.local and /status work; `"wifi":{"mode":"off"}`.
- Firmware uploaded to `/firmware/` through the web page's file API (fsweb: CRC checked).
- `wifi=on` at runtime: `chip 0x4345 rev 6`, `firmware ready`, `BCM43455 up in 713 ms`. The first scan found
  10 networks in 5.1 s (the strongest -33 dBm, all on 2.4 GHz channels 1-11). `wifi=scan` again worked. Ethernet
  and sidecar.local (mDNS + HTTP 200) kept working with the chip up.
- Core 0 (the new `/status` `core0`: the main loop's `MsSleep (1)` in us, per read): wifi off: average 1001,
  max 1003-1121. Chip up and scanning: average 1005-1008, max 1072-1869. The loop rate did not change, the
  temperature stayed at 53-54 C, and there were 0 underruns (drone=0, but with no stream the voices do not render).
  **Still to do:** the same with the Rytm streaming (pi_run.py gaps, `sidecar:` irq gap and handler max) and with a
  joined network carrying traffic.
- Join: not tested (it needs the user's passphrase).
- Left as found: `wifi=off` (wifi.txt on=0), then the PoC kernel flashed again with Wi-Fi off.

## The PoC (pi/engine/wifi.h)

- cmdline.txt `wifi=0` (default) | `scan` | `1`. With no `wifi=` in cmdline.txt, wifi.txt's `on=1` means `1`.
  `wifi=1` without a stored network acts as `scan`.
- `/.rytm/wifi.txt`: `on= ssid= psk= country= [band=2g|5g|any] [ip= netmask= gw= dns=]`. `band=` (default 2g; console `wifi=band:`) sets the network block's `freq_list`: measured 2026-10-17, on a 5 GHz DFS channel (5530 MHz) the handshake completed but DHCP never answered.
- Console (serial `:` and web `/cmd`, use POST): `wifi`, `wifi=scan`, `wifi=on`, `wifi=off`,
  `wifi=join:SSID:PASS`. The SSID ends at the first `:`. on, off and join apply at the next boot; `wifi=on` in the off
  mode also starts the chip for scans now.
- `/status` → `"wifi":{"mode","chip","stored","on","connected","scanning","scans","networks":[{ssid,rssi,ch}]}`, and
  `"ip"` is the DHCP lease on Wi-Fi.
- `/status` → `"core0":{"sleep1msAvgUs","sleep1msMaxUs","loops"}` (since the last read).
- `CNetSubSystem` is constructed in `Initialize` (placement new into `m_NetStore`), after the card is mounted, so
  wifi.txt can choose the device.

### Hardware test steps

1. Flash (RAM): `CIRCLE_BUILD=… CIRCLE_TREE=circle-engine ./flash.sh ../engine 60` from `pi/circle`, with `wifi=0`
   (no change in cmdline.txt). Ethernet, sidecar.local and `/status` must work as before (`"wifi":{"mode":"off"…}`).
2. Upload the three firmware files to `/firmware/` (the web page's Files, or the fsweb API).
3. `:wifi=on` (web or serial): the chip starts (`BCM43455 up in N ms`), then it scans and logs `scan: N networks…`.
   Compare core 0's load, the temperature, `under` and `fifo` before and after (with `drone`, or the Rytm streaming).
   Ethernet and sidecar.local must still work.
4. Join, with the user typing it: `:wifi=join:<SSID>:<passphrase>` → `network "<SSID>" stored`. Then reboot
   (a RAM kernel: flash again). wifi.txt's on=1 picks Wi-Fi: `joining "<SSID>"`, then `http://sidecar.local/` over
   Wi-Fi (Ethernet unplugged), `/status` "connected":true with the lease's IP.
5. Back: `:wifi=off` (or `wifi=0` in cmdline.txt) and reboot → Ethernet.
