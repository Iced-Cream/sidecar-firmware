# Starter content

The starter content zip of a Sidecar release (`sidecar-content-<VERSION>.zip`): patch libraries for the JUNO and
6-OP FM machines, extracted to the root of a Sidecar's or a Sidecar Mini's SD card.

The data files are not stored here; they are generated from this tree's own sources:

```
content/make_content.sh [VERSION]
```

- `JUNO/Factory/`: the Juno-106's 128 factory patches, from `sim/juno/juno_patches.h` (generated from
  shorepine/amy `amy/juno.py`, MIT).
- `DX7/`: the three 32-voice DX7 banks of Mutable Instruments Plaits (`plaits/resources.cc`, MIT), which
  `sim/plaits/build.sh` fetches at a pinned commit.
- `README.txt`, `NOTICE.txt`: the templates in this folder (`@VERSION@`, `@COMMIT@` are filled in).

Output: `build/content/sidecar-content-<VERSION>/`, the zip next to it, and `SHA256SUMS`. The release's own copy is
made by the same generator (`scripts/package_content.sh` in
[sidecar-expander](https://github.com/trigcondition/sidecar-expander)).
