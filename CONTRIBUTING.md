# Contributing

Thanks for helping with Sidecar. Bug reports, tests on hardware and fixes are welcome. This fork's engine is generic services only (`docs/pi-engine.md`): a feature for one Rytm mod belongs in that mod, reaching the Pi through the generic commands.

`main` holds the releases, `dev` is the development branch: work goes on a feature branch off `dev`, and pull requests go
to `dev`. Releases reach `main` from `dev`, and the maintainer merges those. Feature branches are public too. `dev` moves and can be
broken; it is not a release. On every branch: no firmware (see Rules), and run the tests below before you open the pull
request.

## Workflow

1. **Fork** this repository on GitHub and clone your fork, then `git checkout dev`.
2. Create a branch for your change from `dev`: `git checkout -b fix-scope-meter dev`.
3. Make the change, with a test where the code has one (`core/tests`, `pi/engine/tests`, ...).
4. Run the tests (below) and make sure they pass.
5. Push the branch to your fork and open a **pull request** against `dev`. Fill in the template.
6. The `test` check must pass, and a code owner reviews and approves before the pull request is merged.
   `main` takes no direct pushes or force pushes.

Keep pull requests focused: one fix or feature each. Say in the description what you tested it on (the emulator,
Sidecar, a real Rytm).

## Tests

```
tools/run_portable_tests.sh     # Linux or macOS: the "test" check (the core, the Pi engine's portable parts, the page)
```

## Rules

- **No Elektron firmware, and nothing derived from it**: no `.syx`, OS sections, memory dumps, snapshots, card
  images, and no code or data copied out of the Rytm's OS. `.gitignore` covers the usual file types; check your diff.
  The Rytm's USB descriptor values (what a USB host must know to talk to it) are the one exception.
- **No personal data** in code, docs or commits: no names you do not want public, e-mail addresses, home-directory
  paths or Wi-Fi settings. Use paths relative to the repository.
- **Licences.** Our code is GPL-2.0-or-later. Third-party code must keep
  its notice and have a licence compatible with that; add it to `NOTICE`. Prefer fetching third-party sources at a
  pinned commit (as `pi/circle/build.sh` does) to copying them in.
- By contributing you agree that your contribution is licensed under the licence of the files it changes
  (GPL-2.0-or-later unless the file says otherwise).

## Reporting bugs

Open an issue with the bug template: the Sidecar version, the Rytm build (e.g. `rui1027d`), the platform, and steps to
reproduce. Do not attach firmware files.
