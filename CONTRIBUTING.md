# Contributing to NickelDissolve

Technical guide for building, testing, and changing the mod. For how the wipe actually works (the platform table, waveform/CFA handling, per-device defaults, and every `nds_debug_*` key), read [ABOUT.md](ABOUT.md), the technical companion to the (deliberately non-technical) README. 

These mods follow the shared conventions in [NickelGuidance](https://github.com/nicoverbruggen/NickelGuidance), which may not be available at the time of publishing.

## Building

Needs [podman](https://podman.io) (or Docker); the ARM cross-toolchain runs in a container, so your host never needs it:

```sh
git clone --recursive https://github.com/nicoverbruggen/NickelDissolve # --recursive: NickelHook is a submodule
cd NickelDissolve
./build.sh # make clean all strip koboroot in ghcr.io/pgaskin/nickeltc:1.0
```

This produces `KoboRoot.tgz` at the repo root. `./build.sh <targets>` passes other make targets through; `NICKELTC_IMAGE` overrides the toolchain image.

Version stamping: NickelHook.mk bakes `git describe --tags --always --dirty` into `NH_VERSION`: the git **tag** when you're on one, otherwise a **commit hash**. `build.sh` excludes `.git`, so local container builds are unstamped (`dev`); CI (checkout with `fetch-depth: 0`) produces the authoritative stamped artifacts.

## Testing on a device

1. Copy `KoboRoot.tgz` into the Kobo's hidden `.kobo` folder over USB.
2. Eject and reboot; the firmware installs it and deletes the tgz.
3. The mod's folder is `KOBOeReader/.adds/nickel-dissolve/` (`doc`, `uninstall`, and once it
   logs, `nickel-dissolve.log`). There is no shipped config file; the built-in defaults apply
   until you create one.

**Boot safety / recovery**: NickelHook's failsafe (`failsafe_delay = 3`) uninstalls the mod if Nickel crashes within ~3 s of boot. Power off within that window to recover a bad build. Deleting the `uninstall` file in the mod folder and rebooting also removes it.

## Logs & debugging

The mod logs to `KOBOeReader/.adds/nickel-dissolve/nickel-dissolve.log` (and to syslog via `nh_log`, viewable with `logread` on a shell-enabled device). Every message carries the mod version; the startup block logs the mod version, the firmware version, and the resolved settings.

Logging is tied to the **mode** so a working device stays quiet:

- `nds_mode:sweep` (default) with a clean config: only the startup block.
- `nds_log:1`: verbose page-turn / sweep traces (`SWEEP`/`SKIP`/`turn`, and the ioctl stream).
- `nds_mode:observe`: passthrough (no animation) plus the full ioctl stream; use this to study a new device's e-ink traffic.
- A malformed config turns verbose logging on for that boot automatically, so mistakes self-diagnose.

The log is size-capped (256 KB) and rotates once to `nickel-dissolve.log.old`.

## Firmware compatibility

The two hooked `libnickel` symbols (`ReadingView::next/prevPageWithTimer`) carry `//libnickel <first> <last|*> <symbol>` annotations. The `test/syms` checker (CI job `syms`, also runnable locally with Go: `cd test/syms && go build -o ../../test.syms . && cd ../src && ../test.syms`) verifies them against ~70 real firmware dumps (4.6 → 4.45). The e-ink `ioctl` hook lives in `libkobo.so` and is matched at runtime by ioctl number, so it isn't covered by that checker. An unrecognised platform simply passes through untouched.

All hooks are `.optional = true` and null-checked at the use site: a missing symbol makes a feature inert, never fatal. Keep it that way. Targets Kobo **4.x** firmware only; 5.x (Qt 6 / Chromium) is out of scope and the mod stays inert there.

## Pull requests

- Add a `## Unreleased` entry to `CHANGELOG.md` for any user-visible change (release notes are generated from it).
- Annotate any new `libnickel` symbol with `//libnickel …`; CI verifies it.
- State the device + firmware you tested on, and attach the relevant `nickel-dissolve.log` excerpt (the PR template asks for both).

## Releases (maintainers)

Rename `## Unreleased` in `CHANGELOG.md` to the new `## vX.Y`, tag the commit `vX.Y`, and push the tag. CI builds, extracts that section as the release notes, attaches `KoboRoot.tgz`, and fails if the CHANGELOG section is missing.