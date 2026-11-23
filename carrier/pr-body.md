What this changes, and why:

A storage field in mod.json can take `"offset": N`, its first bit in the scope, and the allocator places it there in every lockfile, the first build's too. A mod moving data that an older build of its own kept in these bytes can then pin its fields to those bits, so a project saved by either build reads the same. This is for #423: sidecar-mods' arp code (10 bits) and machine tag (14 bits) at sound offsets 30 and 42 land on stored +0x78..+0x7b, the slots their current build reads. A hand-written lockfile can't do this, because each user's build (web page or app) keeps its own.

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

Tests that ran (docs/TESTS.md):

- `python tests/test_store.py`, `python tests/test_link_refusals.py` (24 passed), `python tests/test_units.py` (85 passed)
- `python tests/test_drive_link_store.py` (C boot linker and twin), `test_drive_store.py`, `test_drive_store_host.py`, `test_drive_reserve.py` and every other test file that needs no stock file: all pass except `test_drive_claims.py`, which fails the same way on main (`build_host()` arguments)
- `node --test "js/test/*.test.ts"`: 197 passed, 2 skipped
- `doc_audit.py --strict --ratchet origin/main`: nothing got worse
- Not run: the tests that need stock files (`test_link.py`, `test_patcher.py`, `test_rytm_mk2.py`) and the js parity run. This hasn't run on a unit yet.
