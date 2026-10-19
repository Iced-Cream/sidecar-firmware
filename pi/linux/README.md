# Pi 4 on Linux (the fallback route of the Pi spike)

Prototype of `docs/pi-circle-spike.md` "Fallback: the Linux route", modelled on AbletonOS (Move).

| Part | What |
|---|---|
| `engine/` | C stub engine: RT audio thread (SCHED_FIFO 80, mlockall, cpu 3, 48 kHz clock, 7/64/128-frame blocks, wake-up latency histogram), Overbridge host on libusb (VID 1935 PID 0b48, alt 3, EP 0x83/0x03, 368-byte blocks, gap/underrun counters), USB-MIDI SysEx `F0 7D 52 54` parser (logs cmd 0x04/0x03). No Rytm attached: says so and runs the timing test only. |
| `rt-tuning/` | `S10rt-tune` (SysV): IRQs + workqueues on cores 0-2, USB IRQ (chrt 90) and audio/DMA IRQ (chrt 91) on core 3, governor performance, RT throttling off. `cmdline.txt` (commented) / `cmdline.txt.oneline` (isolcpus=3 nohz_full=3 rcu_nocbs=3 ...). `S90engine` starts the engine. |
| `buildroot/` | br2-external on Buildroot 2026.02.3 (LTS): Raspberry Pi kernel stable_20260911 (6.18.50) with PREEMPT_RT, BusyBox init, libusb, rt-tests, dropbear, engine, rt-tuning, 115200 serial console (ttyAMA0), genimage boot + A/B root. Dev image: root password `rytm`. |

## Build / run
```
# engine, in an arm64 container (same arch as the Pi 4)
docker build -t rytm-pi-engine engine/
docker run --rm -v "$PWD/engine":/src rytm-pi-engine make
docker run --rm --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit rtprio=99 --ulimit memlock=-1 \
  -v "$PWD/engine":/src rytm-pi-engine ./engine --period 7 --seconds 10     # 7 | 64 | 128

# image (Buildroot tree lives in docker volume rytm-br; 1-2 h first time)
buildroot/build.sh          # then: buildroot/build.sh fetch   -> out/sdcard.img
```

## Docker-on-macOS numbers (10 s each, Apple Silicon, 12 CPU VM; NOT an RT kernel, meaningless for the Pi)
| block | avg wake-up | p99 | p99.99 | max | late > 1 period |
|---|---|---|---|---|---|
| 7 frames (145.8 us) | 36.6 us | 48 us | 536 us | 1376 us | 17 of 68572 |
| 64 (1333 us) | 177 us | 350 us | 2000 us | 6950 us | 8 of 7501 |
| 128 (2667 us) | 429 us | 655 us | 1054 us | 2429 us | 0 of 3751 |

## Needs the real Pi + Rytm
Real RT jitter (PREEMPT_RT, isolated core 3), USB high-speed interrupt timing through the VL805/xHCI, Overbridge gaps/underruns under load, boot time (target 5-8 s).

## On the Pi (flashed sdcard.img, Rytm in OVERBRIDGE mode on a blue USB 3 port)
```
dd if=out/sdcard.img of=/dev/rdiskN bs=4m          # macOS; check N with diskutil list
ssh root@rytm.local   (or the serial console, 115200)
/etc/init.d/S10rt-tune status                       # prio 90/91 on irq threads, affinity masks
engine --period 7 --seconds 60                      # block clock + Overbridge + MIDI trig log
cyclictest -m -p 80 -a 3 -t 1 -i 146 -D 60 -q       # reference jitter (rt-tests)
taskset -c 0-2 sh -c 'while :; do :; done' &        # load on the other cores, repeat the above
```

## Status (2026-10-11)
- Buildroot image builds (aarch64 host, ~2 h cold): Image 6.18.50-v8 `SMP PREEMPT_RT`, sdcard.img with boot + A/B root.
- QEMU `-M raspi4b` boots the kernel but its SD/MMC model corrupts reads (rootfs on SD fails), so smoke-test with the rootfs as initramfs:
  `qemu-system-aarch64 -M raspi4b -m 2G -kernel out/Image -dtb out/bcm2711-rpi-4-b.dtb -initrd rootfs.cpio.gz -append "console=ttyAMA1,115200 rdinit=/init" -serial stdio -display none`
  (PL011 is ttyAMA1 there; on the real Pi with `miniuart-bt` it is ttyAMA0). No USB/PCIe in that machine; IRQ affinity is refused. Not a timing reference.
- Untested on hardware: S10rt-tune's IRQ name matching (check `S10rt-tune status` on the Pi and adjust USB_RE/AUDIO_RE).
