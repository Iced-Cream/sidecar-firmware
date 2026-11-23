**Draft: not run on a unit yet.** Two parts for [#423](https://github.com/elekloader/elekloader/issues/423), in three commits: a storage field's `offset` (pins), then core-ar2 0.20's `ev_store_adopt`, then one docs line that ties them together.

## Part 1: a field's `offset`

A storage field in mod.json can take `"offset": N`, its first bit in the scope, and the allocator places it there in every lockfile, the first build's too. A mod moving data that an older build of its own kept in these bytes can then pin its fields to those bits, so a project saved by either build reads the same. This is for [#423](https://github.com/elekloader/elekloader/issues/423): sidecar-mods' arp code (10 bits) and machine tag (14 bits) at sound offsets 30 and 42 land on stored +0x78..+0x7b, the slots their current build reads. A hand-written lockfile can't do this, because each user's build (web page or app) keeps its own.

- **Repacks keep pins.** A repack places the pinned fields at their offsets and packs the others around them, each at the lowest bits clear (mod, sym order; widest first where that leaves one out). Another mod adding a sound or kit field no longer moves a pinned field. With no pins, the order and bits are the ones it has always packed: the existing store vectors are unchanged.
- **Gaps.** A new field may take the gaps pins leave below `next`, since those bits never held anything. A pin on another field's bits, or on a freed field's, repacks the scope.
- **Moving a pin.** A field whose `offset` changes moves there, and its data is dropped (note: "moved to its offset").
- **Refused:**
  - two pinned fields on one bit;
  - a pin that ends past the scope's room with the build's core (core's own bytes lie past it), naming the core that has the room;
  - fields that don't fit around their pins;
  - an `offset` that isn't a whole number, 0 or more.
- **+Drive.** +Drive mods can't pin a field: the boot linker already refuses `offset` as a format error. This is now tested, C and twin.
- **Lockfile.** The format doesn't change. A pin is not stored: a field keeps its place while its place is its offset. An older elekloader refuses a mod.json with `offset` (an unknown storage key).
- **Ports and docs.** js/ has the same change in store.ts and link.ts, and store-vectors.json gains `pin_runs` and offset decls. docs/FORMAT.md and docs/ADAPTING.md ("Mod storage (core-ar2 0.2, Analog Rytm MKII)") are updated.

### Tests that ran (docs/TESTS.md)

- `python tests/test_store.py`, `python tests/test_link_refusals.py` (24 passed), `python tests/test_units.py` (85 passed)
- `python tests/test_drive_link_store.py` (C boot linker and twin), `test_drive_store.py`, `test_drive_store_host.py`, `test_drive_reserve.py` and every other test file that needs no stock file: all pass except `test_drive_claims.py`, which fails the same way on main (`build_host()` arguments)
- `node --test "js/test/*.test.ts"`: 197 passed, 2 skipped
- `doc_audit.py --strict --ratchet origin/main`: nothing got worse
- Not run: the tests that need stock files (`test_link.py`, `test_patcher.py`, `test_rytm_mk2.py`) and the js parity run. This hasn't run on a unit yet.


## Part 2: `ev_store_adopt` (core-ar2 0.20)


core-ar2 0.20 adds `ev_store_adopt`. When a sound or kit loads with a stored layout ID or generation the build doesn't read, `st_sound_load` / `st_kit_load` still zero its fields. Then, before anything reads them, core calls each handler with `f(scope, live, src, id)`:

- `scope`: `STORE_SOUND` or `STORE_KIT`
- `live`: the live item
- `src`: the stored sound (162 B) or kit (2610 B) it came from, read only
- `id`: the layout ID `src` carries, or 0 if it has none

A mod that recognises its own older bytes in `src` sets its fields with `core_store_set`.

- **No new sites.** The call sits inside the two load sites core already owns, so it covers every path a sound or kit loads: a project, the boot, a library or pool sound, a SysEx dump, a copy.
- **No write-through during adoption.** `core_store_set` on the item being adopted skips the write-through, so nothing is written into the stored project or the source mid-load. The item takes the build's ID at its next save or write-through, so it is adopted once. Until then each load adopts it again from the same bytes.
- **No subscriber, no change.** With no handler the loads behave as in 0.19, apart from one extra table test.
- **Storage fields only.** Lock rows and scene entries of another layout are dropped as before.

It comes from issue [#423](https://github.com/elekloader/elekloader/issues/423). sidecar-mods keeps its arp code, machine tag and FX words in stored sound and kit bytes from before core storage, and this lets it take them over once when it ports to `core_store`. With its fields pinned by `offset` (part 1), the handler has nothing to move and its older build still reads the same projects.

### Changes

- `mods/core-ar2/store.c`: `adopt()` and its calls in `st_sound_load` / `st_kit_load`; `core_store_set` skips the write-through for the adopting item
- `mods/core-ar2/mod.json`: 0.20, the table `ev_store_adopt` (256 spare, for +Drive mods), description
- `elekloader/sdk/include/analog-rytm-mk2/store.h`: the handler's contract and typedef
- `docs/ADAPTING.md` ("Mod storage (core-ar2 0.2)") and `docs/CORES.md`
- `examples/adopt-check-ar2`: the probe. It adopts the first sound and kit bytes of another layout (store-check's), and shows the counts and whether the current sound and kit hold adopted values.

### What ran

- `python tests/test_units.py`: 85 passed
- `python tests/test_link_refusals.py`: 23 passed
- `doc_audit.py --strict --ratchet origin/main`: nothing got worse
- `store.c` and the example compiled with the Rytm toolchain's flags plus `-Werror` (m68k-linux-gnu-gcc 13.2): clean. `store.o` grows by 248 B of text.
- **Not run:** the core build, the link, and `tests/test_rytm_mk2.py` / `test_sdk.py` with a stock 1.74 file; the emulator; a unit
