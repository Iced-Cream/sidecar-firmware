# Box project bundles (generic two-box line)

One bundle per box project, `/Bundles/<name>.sidecar.zip` on the Pi's card (core 0 writes it). Scope decided
2026-11-28 (generic two-box scope, section 8 "Bundles"; port step S8n). The box's own mod (sidecar-main) starts an
export and streams the box project (kits, patterns, sounds, through sidecar-core) to the Pi; the Pi adds the
project's STORE folder, the pool samples it uses and the hashes of the blobs and web bundles it needs, and packs the
zip. An import runs the other way.

Built so far (the portable part, `core/bundle`, host tests only): the zip container, the manifest, the pack layout and
the import's checks. Still to come: the generic bulk upload (code shipping's chunked transport, generalised), the
Pi's pack / unpack service, and the listing of a project's STORE records and pool samples.

## Format

- **Container** (`core/bundle/zip.h`, unchanged from the old line's bundles): a plain stored ZIP any Mac or PC opens.
  No compression, no data descriptors, no ZIP64 (at most 4 GiB - 64 KiB and 65535 entries), UTF-8 names, every entry's
  data on a 4096-byte boundary (extra field 0xD935). `render()` makes any byte range from the entry list, so the same
  code writes a bundle to the card and could serve a download's ranges. The reader refuses anything else.
- **Entries** (`core/bundle/pack.h`): `manifest.json` first, then the manifest's entries in its order:
  - `box/<name>`: what the box's mod streamed over, opaque to the Pi;
  - `store/<scope>-<handle, 4 hex>.bin`: the project's STORE record files;
  - `samples/<path>`: a pool sample, `<path>` its card path under `/Samples`.
- **Manifest** (`core/bundle/manifest.h`): `format` `sidecar-bundle`, `version`, `min_reader`, `created`, `clock`,
  `sidecar` {version, board}, `box` {type (`rytm` `a4` `digitakt`, the device state folder without its dot), uid (DeviceUID, 8 lowercase hex), name}, `project` {id
  (the STORE project ID, 8 lowercase hex), name}, `needs` [{kind `code` or `www`, name, sha256}] (never packed: the
  import asks for any the Pi lacks), `entries` [{name, kind `box` `store` `sample`, size, sha256, paths (samples),
  format {name, version}}], `missing` (samples named but not on the card), `totals`.

## Versions

The manifest is the format registry's `bundle` row (`core/fmt/formats.cpp`): the writer writes the current version
only (2), and `read_manifest()` takes `fmt::load()`'s verdict first. A newer bundle whose `min_reader` this build
meets is read but marked read-only; one past it is refused ("made by a newer Sidecar"). Version 1 was the old line's
Rytm project bundle (slots, prints, sqp patterns): a different content with no upgrade, refused as an older version.
An entry's own format (STORE records: `store`) is checked against its registry row too; an unregistered name is
carried as it is.

## Import checks

`sniff_zip()` finds `manifest.json` in the bundle's first bytes, so the manifest is read before the central
directory. `check()` then refuses a zip whose entries are not exactly the manifest's (an extra, a missing or a reordered
entry, a size that differs, a name outside its kind's folder), and `EntryHash` matches every entry's size, CRC-32 and
SHA-256 as its bytes arrive: one wrong byte refuses the bundle before anything is written. manifest.json's own CRC (the
zip's first entry) is the unpack service's to compare with the bytes it read (the test does it).

Tests: `core/tests/test_bundle_zip.cpp` (`make -C core test`). Python's `zipfile` and `unzip -t` checks are skipped,
with a line saying so, where those tools are missing.
