# Contributing

Thanks for helping with Sidecar. Bug reports, tests on hardware, fixes, machines and effects are all welcome.

## Workflow

1. **Fork** this repository on GitHub and clone your fork.
2. Create a branch for your change: `git checkout -b fix-scope-meter`.
3. Make the change, with a test where the code has one (`core/tests`, `pi/engine/tests`, `web/*.test.mjs`, ...).
4. Run the tests (below) and make sure they pass.
5. Push the branch to your fork and open a **pull request** against `main`. Fill in the template.
6. The `test` check must pass, and a code owner reviews and approves before the pull request is merged.
   `main` takes no direct pushes or force pushes.

Keep pull requests focused: one fix or feature each. Say in the description what you tested it on (the emulator,
Sidecar Mini, Sidecar, a real Rytm).

## Tests

```
tools/run_portable_tests.sh                  # Linux or macOS: the "test" check
make -C core engines && make -C core test    # macOS: the engine core ("core" check); ends with ALL CORE TESTS PASS
```

The engine sources are fetched at pinned commits on first use (network needed once).

## Rules

- **No Elektron firmware, and nothing derived from it**: no `.syx`, OS sections, memory dumps, snapshots, card
  images, and no code or data copied out of the Rytm's OS. `.gitignore` covers the usual file types; check your diff.
  The Rytm's USB descriptor values (what a USB host must know to talk to it) are the one exception.
- **No personal data** in code, docs or commits: no names you do not want public, e-mail addresses, home-directory
  paths or Wi-Fi settings. Use paths relative to the repository.
- **Licences.** Our code is GPL-2.0-or-later; the starter machines in `plugins/sc_*` are MIT. Third-party code must keep
  its notice and have a licence compatible with that; add it to `NOTICE`. Prefer fetching third-party sources at a
  pinned commit (as `sim/plaits/build.sh` does) to copying them in.
- By contributing you agree that your contribution is licensed under the licence of the files it changes
  (GPL-2.0-or-later unless the file says otherwise).

## Reporting bugs

Open an issue with the bug template: the Sidecar version, the Rytm build (e.g. `rui1027d`), the platform, and steps to
reproduce. Do not attach firmware files.
