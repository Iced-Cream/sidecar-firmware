# AGENTS.md: sidecar-firmware

Guide for AI coding agents (and the humans who run them) working in this repository. It is the Sidecar engine for the
Raspberry Pi 4B / Pi 5: the voices, effects and sequencer, the touchscreen UI, the web page, the update path and the
protocol the Rytm mods talk to. Read it before you change anything. `CONTRIBUTING.md` has the process for people;
this file has the commands, the rules and the traps.

Where this file says "your human", it means the person whose Pi, Rytm and GitHub account you work for. You decide
nothing about their hardware without them.

Items marked **(not verified)** were not run while this file was written. Run them yourself before you rely on them.

## 1. Hard safety rules

These never bend. If a task seems to need one broken, stop and ask your human.

| Rule | Detail |
|---|---|
| Never flash a Rytm | Not from this repo, not "just to test". Flashing is your human's act, after the mods repo's emulator checks. |
| Never push a firmware image to a device without an explicit go | A kernel, a `.scu`, an SD image: write what you will send and where, wait for a clear "yes" in the chat, then send it. A yes covers one install, not the session. |
| Kernels only for your human's own Pi | Build and install kernels only for a Pi your human owns and named. Never publish a kernel, `.scu` or SD image; the maintainer builds releases. Do not commit them either (`*.img` is git-ignored). |
| No Elektron code or firmware bytes | No `.syx`, OS sections, memory dumps, snapshots, card images, or code or data copied out of the Rytm's OS. The Rytm's USB descriptor values are the one exception. |
| Clean room | Never read leaked or decompiled vendor source, or third-party reverse-engineered code whose licence is incompatible with GPL-2.0-or-later. Sources under MIT-style licences (Mutable Instruments, Airwindows, 4ms SMR and similar) may be read and credited in `NOTICE`. When unsure of a licence, treat the code as unreadable and ask. |
| No personal data | No names, e-mail addresses, home-directory paths, Wi-Fi names or keys, serial numbers or pairing codes in code, docs, commits, PR text or logs you paste. Use repo-relative paths and placeholders such as `<your-pi-ip>`. |
| Only `.elemod` files go to testers | Rytm mods reach testers and users as `.elemod` files from the mods repo's releases. Never attach or link a `.syx` (a firmware image) in a pull request, issue or release. |
| No secrets in URLs, logs or files | The pairing code, session tokens and client secrets never go into a URL, a log line, a commit or a file in the repo. See section 7. |
| Never `--no-verify` | Do not skip hooks. If a hook refuses, fix the cause. |
| Never `git add -A` or `git add .` | Add files by name. Check `git status` and `git diff --cached` before every commit. |
| Never push to `main` or force-push | Pull requests go to `dev`. The maintainer merges. |

## 2. What this repository is

| Path | What |
|---|---|
| `core/` | The portable engine core (C++17): voices, machines, send FX, LFOs, presets, the remote UI pages, the host link, device profiles. Host tests in `core/tests`. |
| `pi/engine/` | The Pi's bare-metal engine on Circle: USB, network, web server, update, SD card, the kernel entry. Portable tests in `pi/engine/tests`. |
| `pi/circle/` | Circle build scripts and the patches applied to it. `pi/engine/build.sh` applies them. |
| `pi/ui/` | The touchscreen UI (LVGL) and the remote-UI protocol library (`pi/ui/remote`). Host build in `pi/ui/host`. |
| `web-next/` | The web page (Vue 3 + Vite + TypeScript) over the `/ws` WebSocket. Tests with vitest. |
| `pi/engine/www/` | The classic web page and the page's built-in assets. |
| `web/`, `teensy/` | Sidecar Mini (Teensy 4.1). Frozen at 0.1.0: no new features. |
| `plugins/`, `sdk/` | User machine and effect plugins, and the plugin SDK (ABI header, linker scripts). |
| `sim/` | Mac-side simulator and end-to-end tools. `sim/protocol.py` is the wire protocol reference. |
| `tools/` | Packaging (`make_update.py`), versioning (`gen_version.sh`), plugin pack and install, the portable test runner. |
| `docs/` | Design notes and measurements. Start with `docs/core.md`, `docs/pi-engine.md`, `docs/web-update.md`, `docs/versioning.md`. |
| `third_party/`, `licenses/`, `NOTICE` | Vendored sources and their licences. |

The version is in `VERSION` (`docs/versioning.md`). One number covers the Pi engine and the shared core.

Third-party engine sources (Plaits, DaisySP, Circle, LVGL and others) are not vendored. The setup scripts fetch them at
pinned commits into git-ignored folders. The first build needs the network.

## 3. How this repo and the mods repo meet

The Rytm side lives in [sidecar-mods](https://github.com/trigcondition/sidecar-mods) (Analog Rytm MKII OS mods). The two
repos talk over SysEx on the Rytm's USB MIDI port, manufacturer ID `0x7D`, header `F0 7D 52 54 <cmd> ... F7`.

| Where | What it defines |
|---|---|
| `sim/protocol.py` | Every message: command byte, layout, direction, which caps bit gates it. The reference. |
| `docs/core.md` | The engine core's side of the link. |
| `docs/remote-ui.md` | The remote-UI protocol (the open protocol for clients that draw the Rytm's pages). |
| `mods/*/design.md` in sidecar-mods | The Rytm side of each message and the addresses it touches. |
| `docs/versioning.md` | The version scheme. A release's notes name the Rytm build it needs. |

Compatibility rules:

1. **The Pi must run with the previous Rytm release.** The Rytm's hello carries caps bits; a feature that needs a Rytm
   change is gated by its caps bit and stays off without it. Do not assume the newest mod is flashed.
2. **The Rytm must run with the previous Pi release.** A receiver ignores a message version or command it does not
   know. New messages are additive: new command bytes, new trailing bytes, new caps bits.
3. **Never reuse or reorder** an existing command byte or field. Append.
4. A change on both sides is **one issue and two linked pull requests**, one per repo. Both must pass with the other
   side's previous release: your Sidecar with the previous mod release, and an older Sidecar with your mod.
5. **Document a protocol addition in the same pull request**: what it carries, who sends it, what an older side does
   with it. Update `sim/protocol.py` (and its tests) and the mods repo's `design.md`.
6. Test the host side against the emulator and the mods repo's suites only through the mods repo (its `RYTM_TEENSY`
   variable points at a checkout of this repo). See sidecar-mods `AGENTS.md`.

## 4. Build and test

Always run the tests for what you changed before you open a pull request. CI runs two checks:

| Check | Runs | Where |
|---|---|---|
| `test` | `tools/run_portable_tests.sh` | Linux (`.github/workflows/test.yml`) |
| `core` | `make -C core engines && make -C core test`, then `make -C core test-lib-san` | macOS (`.github/workflows/core.yml`) |

### Engine core (macOS only)

```sh
make -C core engines        # once: fetches Plaits, DaisySP and others at pinned commits (network)
make -C core test           # last line: ALL CORE TESTS PASS
make -C core test-lib-san   # library walk depth under ASan + UBSan
```

The core tests build on macOS only (Apple's JIT API in `core/host/um_host.cpp`). On Linux you can run the portable
tests only.

### Portable tests (Linux or macOS)

```sh
tools/run_portable_tests.sh   # no hardware, no firmware, no cross toolchain; ends with ALL PORTABLE TESTS PASS
make -C pi/engine/tests       # the Pi engine's portable parts under ASan/UBSan; ends with ALL PI PORTABLE TESTS PASS
```

- Some Pi tests need the core's Mac library (`make -C core engines` first), a Circle checkout
  (`pi/circle/build.sh`) or both. `run_portable_tests.sh` names the ones it skipped. A skipped test is not a pass.
- Node 20 or newer is needed for the `.test.mjs` files (CI uses 22).

### Touchscreen UI on the host

```sh
pi/ui/fetch-lvgl.sh pi/circle/build   # once: LVGL source into pi/circle/build/lvgl-src
make -C core engines                  # the host UI build needs the engines' headers
make -C pi/ui/host test               # the UI's host tests
```

`make -C pi/ui/host` alone makes `shot.png`. `make -C pi/ui/host bounds` checks that every page stays inside the
screen. The host build also needs a Circle checkout for `lv_conf.h` **(not verified end to end: only `make -n` was
run here, which stops at the missing Circle tree)**.

### Web page (web-next)

```sh
cd web-next
npm ci
npm test            # vitest
npm run typecheck   # vue-tsc
npm run build       # vue-tsc + vite build
npm run mock        # a stand-in Sidecar in Node, to try the page without a Pi
```

The page's pairing and update code is under `web-next/src/lib`. Its tests use a mock of the Pi's endpoints.

### Plugins

`make -C plugins/<name>` (for example `make -C plugins/sc_acid`); see `docs/plugins.md`. Plugin versions have three
numbers only; the engine and update file may have four (`docs/versioning.md`).

### Before you push

- Core tests, portable tests and the web-next tests for every area you touched.
- `git diff --cached` has only your files and no binaries.
- A test for every fix, one that fails without it. Prefer extending the nearest test in `core/tests`,
  `pi/engine/tests`, `web-next/src/**/*.test.ts` or `web/*.test.mjs`.

## 5. Build the kernel (your own Pi)

The kernel build runs in Docker (an arm64 Debian image with the Arm GNU toolchain) and writes the image next to the
script. **Build only. Installing is section 7 and needs your human's go.**

```sh
pi/circle/build.sh        # once: the Circle checkout in pi/circle/build (about 3 minutes, needs network)
pi/engine/build.sh        # Pi 4: pi/engine/kernel8-rpi4.img
RASPPI=5 pi/engine/build.sh   # Pi 5: pi/engine/kernel_2712.img
pi/engine/build.sh kernel     # only the engines' sources and the kernel (the tree is already built)
```

What to know:

- `RASPPI=4` (default) or `5`. Switching the board removes the other board's objects, so the next build is a full one.
- The first Pi 5 run with the USB gadget stops at the `hostap` clone. Run it again.
- `CIRCLE_BUILD=<dir>` points the build at another Circle build directory. A git worktree uses it to share the main
  checkout's `pi/circle/build` instead of building Circle again.
- Defaults compiled in: `PI_IP`, `PI_NETMASK`, `EMUHOST`, `GADGET`, `SIDECAR_CH_IN`, `SIDECAR_NAME`. Read the header
  comment of `pi/engine/build.sh` for each. A `cmdline.txt` on the card overrides the network defaults.
- `BUILD_FLAGS=1 RASPPI=5 pi/engine/build.sh` prints the flags it would use and builds nothing. Use it to check a flag
  change cheaply.
- The build stops with `kernel too big` when the kernel outgrows `KERNEL_MAX_SIZE`. A kernel past the limit dies before
  the serial console starts. Do not work around the check.
- **After a `VERSION` bump, `touch core/ui/remote.cpp` before you build.** `core/version.h` is generated by
  `tools/gen_version.sh`, and the Makefiles do not track it as a dependency, so objects that include it are not rebuilt.
  `core/ui/remote.cpp` shows the version on the Rytm's SYSTEM page and is the one to touch.
- `PI5_TESTMODE=1` is for engine test boots only. `make-sd-dual.sh` refuses a kernel with `testsecs=` in it. Never use
  it for anything you install for daily use.
- Pi 5 and Pi 4 builds are separate trees; see `docs/pi5.md` and `docs/pi-engine.md`.

Kernel changes in `pi/circle/*.patch` follow one pattern: a patch gets its own file and its own marker that
`build.sh` greps for, so a tree built before the patch gets it applied and a tree with an earlier version of it is
refused. Do not edit an already-applied patch in place; add a new one that applies on top.

## 6. Make an update file (`.scu`)

The web page's Settings > Update installs one `.scu` file. `tools/make_update.py` makes it
(`docs/web-update.md` has the format and the install protocol).

```sh
mkdir -p build
python3 tools/make_update.py \
  --pi4 pi/engine/kernel8-rpi4.img --pi5 pi/engine/kernel_2712.img \
  --build-web --out build/sidecar-test.scu
python3 tools/make_update.py --check build/sidecar-test.scu
```

- **Both kernels are required** (`--pi4`, `--pi5`, `--out`), and they must carry the same stamp, so build both from
  the same commit (`RASPPI=4` and `RASPPI=5` in turn, without changing the tree in between). Build them from a clean
  tree: a `-dirty` stamp is allowed in a test file but marks it.
- `--check FILE` lists a file's header and parts and verifies the sums. Run it on every file you made.
- `--build-web` runs `npm run build` in `web-next` and puts `web-next/dist` in the file. It needs
  `web-next/node_modules` (`cd web-next && npm ci` once); it installs nothing itself. `--web DIST` takes a `dist`
  you built yourself. Use one of them, not both.
- **A file without a web part leaves the installed web app alone.** If your change touches `web-next` or the page,
  build the file with `--build-web`, or your Pi keeps the old page.
- Do **not** pass `--release`. It is for the maintainer's release builds and refuses kernels without the release
  network address.
- Tests: `python3 tools/tests/test_make_update.py` (also run by `make -C pi/engine/tests`).

## 7. Install a build on your own Pi

Installing changes a device. **Ask your human first, every time.** Say which file (name and sha256), which Pi, and
that the Pi reboots and playing stops for about a minute. Wait for a clear yes.

A Pi that your human has paired works through the web page, the update API, or `sdboot`. Prefer the web page: your human
does it, and nothing secret passes through you.

### The web page (preferred)

1. Open the Pi's page (`http://sidecar.local`, or `http://<your-pi-ip>`). Pair the browser with the code your human
   reads off the Rytm's screen or the touchscreen's STATUS page (Settings > Access).
2. Settings > Update, pick the `.scu`. The page checks it before it sends anything.
3. Install. The Pi restarts into the new version **on trial**: it commits after 30 s of uptime, and any earlier reboot,
   hang or power cut starts the old version again by itself.

### The update API

Use it when your human has said you may and has paired a client for you (below). All of it is in
`docs/web-update.md`.

| Call | Auth | What |
|---|---|---|
| `GET /update/state` | open | JSON: `enabled`, `board`, `version`, `commit`, `trial`, `state`, `msg`, `prev`, `prevVersion`, `commitIn`, `uptime` |
| `POST /update/put?off=&total=&crc=` | paired, same origin | The file in 49152-byte chunks (multipart field `f`; `crc` is the whole file's CRC-32). The page's code in `web-next/src/lib/upload.ts` is the reference. |
| `POST /update/install` | paired, same origin | Checks the file again and stages the trial. Add `force=1` to install the same or an older version. |
| `POST /update/previous` | paired, same origin | The previous kernel (`.prev`) as a trial. |
| `POST /update/discard` | paired, same origin | Deletes the uploaded file. |

Sending a request that changes anything needs the token in an `X-Sidecar-Auth` header and an `Origin` (or `Referer`)
of `http://<the Pi's own host name or address>`. The update endpoints refuse a request without it. A scripted upload
is **(not verified: write a small client from `upload.ts`, and test it on the mock in `web-next/mock` first)**.

**Pairing for a script.** The pairing code is drawn at every boot and shown only on the Pi's screens. Your human types
it once; you never ask for it in chat and never log it. A client that asks to be remembered gets a client id and a
secret. Keep the secret in a file with mode 0600 outside the repo, and trade it for a token at each run:

```sh
ANSWER=$(curl -s -d client=<client-id> --data-urlencode secret@"$HOME/.config/sidecar/deploy.secret" \
  http://<your-pi-ip>/pair/resume)  # JSON; take the session token out of it (docs/pi-engine.md)
# send that token as the X-Sidecar-Auth header; never echo it or put it in a URL
```

The commands for pairing are in `docs/pi-engine.md` ("A script that uses HTTP"). **The code, the secret and the token
never go into a URL** (`?tok=` is refused), a command line that lands in shell history, a log, a commit or a PR.

### Poll across the reboot

After `POST /update/install` the Pi reboots about half a second later and is unreachable for roughly a minute. Poll
`/update/state`, which is open, until it settles:

```sh
curl -s http://<your-pi-ip>/update/state
```

Read `state` and `msg`. The sequence is `staging`, `trying` (after the reboot, the page says "permanent in N s"),
`committing`, `committed`. `reverted` or `failed` means the old version is back; read `msg` for the reason. Poll every
few seconds, give up after about three minutes, and do not retry the install while a job is `staging`, `trying` or
`committing`. Then check `/status` for the new `version` and `commit`, and `underruns` (should stay 0 while playing).

### Roll back

A rollback is an install too: it needs the same explicit go. Reinstall the previous `.scu` the same way, or `POST /update/previous` (the page's "Back to ..." button). A previous
version from before 0.5.0 cannot commit itself and is swapped in at once; that is the rare case where the way back up
is an SD card in a computer, so check `/update/state` `prevVersion` first. Keep the last known-good `.scu` on disk
before you try a new one.

### Diagnosis

| Endpoint | Auth | Use |
|---|---|---|
| `GET /status` | open | The whole status as JSON: version, commit, links, underruns, temperature, store. No secrets. |
| `GET /health` | open | Alive, build, uptime, temperature, supply flags. |
| `GET /log?since=N` | open | The console log from byte N (the first line is `START END`; ask for `since=END` next). A 64 KB ring: it holds about the last 25 s of busy logging. |
| `GET /blackbox?off=N` | paired | The whole log kept on the card (`/.rytm/blackbox.log`), 64 KB a request. The page's Settings > About downloads it, secrets masked. |
| `POST /cmd` with `c=<command>` | paired | The serial console's parser, for example `c=version`. |

`/status` answers only 20 to 40 seconds after a boot, and `/log` rolls. Anything you need about the boot itself comes
from the black box or the crash report (`GET /prevboot`, paired), not from a live poll started late.

A pasted log can carry names (project and sample names). Read it for the lines you need and quote those only.

### `sdboot` and the serial route

`pi/circle/sdboot.py` installs a kernel over the network (TCP 7310) and reboots; it is for a development card that
runs with `sdport` on. Release cards run with `sdport=0`. Use it only on your human's own development card, with their
go. **(Not verified here.)** Prefer the update file.

## 8. Code rules

**Style.** Match the file you are in. Plain C++17; the Pi engine builds with `-fno-exceptions -fno-rtti`. Use fixed buffers, not
heap allocation, in the audio and control paths. Keep functions short. Comments say why, not what. Comments and docs in English, short
sentences, no marketing.

**Pi runtime rules.**

- **No `%f` in `snprintf` / `sniprintf` on the Pi.** The Pi's `sniprintf` has no float support. Print integers (tenths
  or hundredths). The kernel links `sniprintf`, not `snprintf`.
- **The 2 ms control budget on core 3.** Core 3 never touches the SD card. Card work goes through the card queue
  (`core/msamp/cardq`) to core 0. Core 0 never spins in a wait loop. If you add work to the control pass, measure it:
  the engine logs `control: idle section N us, its slowest step <name>` when it overruns.
- **Timing-based tests need headroom.** A test that asserts on elapsed time flakes on a shared CI runner. Assert on
  counts and ordering, or give a generous bound.
- A **new setting** that survives a restart is stored under `/.rytm/`; a **new endpoint** is paired and same-origin
  unless there is a stated reason to open it (`docs/pi-engine.md` "Pairing code" lists the open set and why).
  `/fs/*` reaches content folders only.
- **A new protocol message or caps bit** follows section 3. A new device profile field is data-only with the Rytm's
  values as defaults (`docs/device-profiles.md`).
- Build scripts are written for macOS: some use BSD `sed -i ''`. In a script you add, avoid GNU-only and BSD-only
  flags, or say which you need.

**Sound changes.** A change that moves a default, a range or a kit's sound for existing users needs a line for the
release notes in the pull request.

**Licences.** The code is GPL-2.0-or-later; `plugins/sc_*` are MIT. Third-party code keeps its notice, needs a
compatible licence, and goes in `NOTICE`. Fetch third-party sources at a pinned commit (as `sim/plaits/build.sh`
does) instead of copying them in.

## 9. Working rules

- **Issue first, and claim it.** For anything bigger than a small fix, find or open an issue, say you want it, and wait
  for the `claimed` label and your name on it. The pinned roadmap issue shows what is planned.
- **One topic per pull request**, linked with `Closes #n`. If it depends on another open pull request, say the order.
- **Pull requests go to `dev`.** Branch from `origin/dev`. `main` holds releases; the maintainer merges release pull
  requests. Do not target `main`.
- **Two-sided changes:** two linked pull requests (section 3), compatible with the previous release on each side.
- **Release scope is the maintainer's.** Close to a release, only fixes for it may be taken; a new feature can wait for
  the next one. The roadmap issue shows what is planned; ask if unsure.
- **Review before merge.** The `test` check must pass and a code owner approves. Do not merge your own pull request or
  ask for an admin bypass.
- **Evidence over claims.** The pull request says what you ran and on what: the command, the Pi model, the Rytm OS and
  mod build tag, the Sidecar version and commit. "Should work" is not evidence. If you could not test something, say so.
- **Shared Rytm resources** (RAM, key handlers, parameter records and kit words) are a mods-side matter. The rules are
  in sidecar-mods `CONTRIBUTING.md`, section "Rytm mods: shared resources". Do not copy them here.
- **Documentation moves with the code.** A changed endpoint, setting, message or build flag updates its doc in the same
  pull request.
- **Commits.** Plain messages, imperative, one logical change each. Use the identity your human
  gave you. Never rewrite published history.

## 10. Lessons

Each of these cost someone a day.

| Lesson | What to do |
|---|---|
| **A Pi that boots after the Rytm misses the Rytm's hello.** The hello is sent about 0.5 s after the Rytm's USB port comes up. Caps-gated features then stay off until the Rytm re-enumerates. | Test both start orders (Pi first, Rytm first). Do not treat "feature missing" as a bug in the feature until you have checked caps and replugged the Rytm. |
| **`core/version.h` is not a make dependency.** After a `VERSION` bump the build shows the old version. | `touch core/ui/remote.cpp`, rebuild, and check `/status` `version`. |
| **Both kernels must share a stamp.** `make_update.py` refuses a pair built from different commits. | Build both boards from one commit, one after the other. |
| **A `.scu` without a web part keeps the old page.** Your UI fix seems not to ship. | Build with `--build-web`; check the file with `--check` (it lists a web app part). |
| **`/status` answers late after boot, `/log` rolls.** Boot problems are gone by the time you look. | Use the black box and `/prevboot`. |
| **No `%f` on the Pi.** Float formatting prints nothing or links wrong. | Integer formats. |
| **Core 3 and the SD card.** One card access in the control pass causes audible dropouts. | Go through `cardq`; measure with the control log line. |
| **Kernel size limit.** A kernel over `KERNEL_MAX_SIZE` dies before the console. | Watch the `kernel size: N KB of M KB` line at the end of the build. |
| **Switching `RASPPI` rebuilds everything.** | Keep one worktree per board if you switch often. |
| **A patch already applied is never re-applied.** Editing it changes nothing in an existing Circle tree. | Add a new patch with its own marker. |
| **Known pre-existing failures.** Before you call a failure new, run the same test on `origin/dev` (or the base your branch started from) and compare. Name the failing tests in the pull request either way. | One baseline run per change, kept for the comparison. |
| **Timing tests flake on CI.** `core/tests/test_projload_cost.cpp` is one. | Rerun once. If it flakes twice, say so in the pull request; do not loosen unrelated tests. |
| **zsh does not split unquoted variables on spaces; bash does.** A script that works from bash and breaks when pasted into macOS's default shell (`for x in $LIST`, `cmd $ARGS`) will pass one long argument. | Use arrays, or run the script with `bash`. Quote paths. |
| **Disk fills quietly.** The Circle checkout is about 115 MB per tree, kernel trees multiply per board and flag, Docker images and `node_modules` add up. | Check free space before a build (`df -h .`). Delete only build output you can rebuild, and ask your human before you delete anything else. |
| **Network access in tests.** The first engine build fetches sources. | Run `make -C core engines` once while online; CI does it too. |

## 11. Reporting

When you finish, write the pull request description as:

1. What changed and why (link the issue).
2. What you ran: the exact commands, the machine (macOS or Linux), the Pi model, the Rytm OS and mod tag if any.
3. What you did not run, and why.
4. Protocol, setting or default changes, and a release-notes line for a sound change.

Do not paste logs with names, addresses, tokens or codes in them.
