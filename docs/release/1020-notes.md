# sidecar-1020: internal notes

Branch `fix-1020` off `sidecar-1019-rui` (c3f0b3c): four hardware bugs from the 1019 test. Pairs with rytm-mods
`build/sidecar-1020/` (sfx1020, rui1020). Not merged to main.

1. **Machine list: the arrows must not assign.** Cause (Rytm side, rytm-mods 0021): the list previews a machine on the
   track at every DOWN, the 0x25 slot prints (a scan a second) saw BD's sound, tagged with U1's plugin, sitting on U2,
   U3, U4, and the host resolve filled each empty slot with that plugin (the user got METAL in every slot); a YES on a
   slot with no plugin left the old tag as well. Fixed in the Rytm (no print while a preview is open, the tag off at a
   choice). The host keeps its rule: a slot that a print names with a plugin the slot lacks is filled. Test:
   `sim/um_e2e.py srcsteps` (the user's key sequence against the real core), rytm-mods `sp_emu.py choose`.
2. **6-OP FM patch name twice** while scrolling: Rytm side (0029's list-knob text), `sfu_emu.py fmname`.
3. **Python crash on the Mac** (SIGSEGV at 0x0 in `jlib::Lib::load_list`): the core's `Lib::log_` (JUNO and DX7
   libraries) was null until `begin()`, which `engine_begin` skips when the card has no /Samples; the Rytm's `10
   <project>` then called it. Both libraries now log through a no-op until `begin()`. Test: `test_core_sysex.py`
   `TestCoreNoCard` (a card without /Samples, a 10 and 1500 random host messages). `sim/rui_e2e.py` was never stuck on
   the open handshake: the run crashed in the core, and its default tag was rui1017 (now rui1020).
4. **Pi `:sendfx` "core 2 load"**: the figure divided render core 2's busy ticks by a 32-bit tick difference (54 MHz:
   wraps every 79.5 s, and the first line was measured from boot), so a line a few minutes after the last one read
   tens of percent for 1-2 %. Now busy (64-bit, monotonic, no read-and-clear against core 2) over the elapsed
   milliseconds. Test: `pi/engine/tests/test_sendfx_pi.cpp` (2000 renders in 5 minutes = 0.012 %).
