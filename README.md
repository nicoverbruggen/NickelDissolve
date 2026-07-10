# NickelDissolve

> [!WARNING]
> **Work in progress — not ready for general use.** This is an experimental prototype under active
> development. Only a few devices are verified, colour-page handling is incomplete, and the config and
> behaviour may still change. Try it only if you know what you're doing and don't mind rough edges.
> (It's reversible and shouldn't brick your device — see [Safety](#safety) — but there are no guarantees.)

A [NickelHook](https://github.com/pgaskin/NickelHook) mod that gives Kobo a **Kindle-style
directional page-turn animation** — the new page sweeps in as a band-wipe across the screen,
instead of a single flat refresh.

> **Status: working prototype on the Kobo Clara BW.** The wipe, greyscale quality, and
> automatic forward/back direction all work on-device. See *Platform support* for other models.

## What it does

The Kindle's smooth page-turn wipe is a **native MediaTek `mxcfb` "swipe" primitive** (direction +
steps, done in hardware). The Kobo is also MediaTek but runs the **`hwtcon`** driver, which has
**no swipe primitive** — so there's no native call to reproduce it.

NickelDissolve approximates it from userspace: it intercepts the single full-screen e-ink
update Nickel issues per page turn and replaces it with a **sequence of partial updates over
vertical strips, swept across the screen**. The framebuffer already holds the new page, so each
strip transitions that band from the old page to the new one — sweeping the bands reads as a
directional wipe. No driver patching; only the driver's public update ioctl.

It's a serial sequence of partial refreshes, so it's a **stepped** wipe, not the perfectly smooth
hardware swipe — but with enough strips it reads well.

## How it works

Three hooks, all resolved by symbol name (so no per-firmware offsets):

- **`ReadingView::nextPageWithTimer` / `prevPageWithTimer`** (in `libnickel`) — fire on a page
  turn. They **arm the sweep** and record the direction (a backward turn sweeps the opposite way).
- **`ioctl`** (in `libkobo`) — when a turn is armed, the next full-screen e-ink update is the page
  render; in `sweep` mode it's replaced with N swept partial-strip updates. Each strip **reuses
  the turn's own waveform** (so whatever the device/page uses — GLR16, REAGL, a Kaleido CFA mode —
  is preserved). The **last strip reuses Nickel's update marker**, so its following
  `WAIT_FOR_UPDATE_COMPLETE` resolves — no hang; a fallback resubmits the original if that strip fails.

**Kobo-agnostic.** The two Kobo e-ink interfaces (`hwtcon` on MTK, `mxcfb` on i.MX) share the same
update-struct prefix, so the mod recognises either by its ioctl number and drives it by field
offset — no per-struct code. Screen dimensions are read from the update itself (resolution-independent).

## Config (`.adds/nickeldissolve/config`)

| key | default | meaning |
|---|---|---|
| `nds_enabled` | 1 | master switch |
| `nds_mode` | `observe` | `observe` = passthrough + log (safe); `sweep` = do the wipe |
| `nds_strips` | 8 | number of vertical bands (2–32). More = smoother + slower |
| `nds_strip_waveform` | 0 | **0 = keep the turn's own waveform** (recommended, works on any device/colour). Non-zero forces a raw, **platform-specific** id (hwtcon: 1=DU 3=GL16 4=GLR16 6=A2; mxcfb differs) |
| `nds_direction` | `rtl` | forward-turn sweep direction (`rtl` or `ltr`); back turns flip automatically |
| `nds_wait` | `submission` | between strips wait for `submission` (fast) or `complete` (slower, more discrete) |
| `nds_delay_us` | 0 | extra pause between strips, µs (0–50000) — the animation-speed dial |
| `nds_tap_animates` | 0 | `0` = only **swipes** animate (a **tap** turns instantly); `1` = taps animate too (see *Swipe vs tap* below) |
| `nds_cfa_skip` | 1 | **Kaleido colour panels:** skip the per-region CFA colour pass on the strips (removes the boundary seams). Correct for B&W; no-op on mono/i.MX |
| `nds_log_ioctl` | 1 | log the ioctl stream / sweep events |

Changes take effect on reboot (config is read once at startup). Tuning guide: leave
`nds_strip_waveform:0` (keep) for best quality on any device; use `nds_strips`/`nds_delay_us`
for smoothness/speed.

## Swipe vs tap

By default, **the animation fires on a swipe but not on a tap** — a tapped page turn is instant.
This is deliberate, and it falls out of how the sweep is triggered: the animation is armed by hooking
`ReadingView::nextPageWithTimer` / `prevPageWithTimer`, which is the path a **swipe** gesture takes.
A **tap**-to-turn goes through `nextPage` / `prevPage`, which are not PLT-hookable (`ABS32`-only), so
a tap never arms the sweep. Many people like this split (a deliberate swipe animates; a quick tap
stays instant), so it's the default.

Set **`nds_tap_animates:1`** to animate taps too. Caveat, because taps can't be hooked individually:
in this mode the sweep is armed by **any** full-screen page render, so (a) it may occasionally
animate a non-turn full-screen refresh, and (b) a tap has no direction signal, so tapped turns reuse
the **last swipe's** direction (or the configured `nds_direction` if you haven't swiped yet).
Recommended only if you turn primarily by tapping and don't mind those trade-offs.

## Safety

- **`observe` (default) changes nothing** — it forwards every ioctl untouched and only logs.
- In `sweep`, **only the full-screen update immediately following a page turn is ever touched**
  (and the arming expires after ~2 s); all other ioctls pass through byte-for-byte, so
  menus/UI/images are unaffected.
- **No hang:** the last strip carries Nickel's marker; a hang-safety fallback resubmits the
  original update if that strip fails.
- Hook is `optional` (firmware mismatch = inert). Worst realistic failure is a garbled frame
  fixed by the next refresh — recoverable, not a brick.
- Revert with `nds_mode:observe`, or delete `.adds/nickeldissolve/uninstall` and reboot.

## Device support

The mod drives two e-ink update interfaces via its platform table — **`hwtcon`** (MediaTek) and
**`mxcfb`** (i.MX). It auto-detects which from the intercepted ioctl, so one build covers both. On
any other platform (or a device not listed) it simply stays **inert** — no animation, no risk.

Target coverage: **Elipsa and newer.**

| Device | SoC / interface | Status |
|---|---|---|
| **Clara BW** (Spa BW) | MTK / hwtcon (mono) | ✅ **Tested** — flawless |
| **Clara Colour** (Spa Colour) | MTK / hwtcon (Kaleido) | ✅ **Tested** (`nds_cfa_skip`, default on) |
| **Libra Colour** (Monza) | MTK / hwtcon (Kaleido) | ✅ **Tested** (`nds_cfa_skip`, default on) |
| **Elipsa 2E** (Condor) | MTK / hwtcon (mono) | 🟡 Same interface — ready, **untested** |
| **Libra 2** (Io) | i.MX / mxcfb (mx6sll) | 🟡 In the platform table — ready, **untested** |
| **Clara 2E** (Goldfinch) | i.MX / mxcfb | 🟡 In the platform table — ready, **untested** |
| **Sage** (Cadmus) | **AllWinner / sunxi** (B300) | ⏳ **Planned** — needs the sunxi code path |
| **Elipsa** (Europa) | **AllWinner / sunxi** (B300) | ⏳ **Planned** — needs the sunxi code path |

The sunxi devices (Elipsa, Sage) use a **different update interface** (`DISP_EINK_UPDATE`, a
pointer-based struct, `frame_id` tracking) that doesn't fit the flat offset table — a dedicated code
path is in progress. Until then the mod is **inert** on them (safe, just no animation).

**Colour devices (Kaleido):** the strip-by-strip sweep would corrupt a *colour* page (the CFA
colour conversion needs the whole frame), so only B&W/greyscale turns are animated cleanly; see
`nds_cfa_skip`. **Colour-page detection/skip is still a TODO.**

**Trying an untested device:** install in `nds_mode:observe` first — it changes nothing and logs
the ioctl stream. Confirm the log shows a platform (`[hwtcon]`/`[mxcfb]`) and `turn:` lines, then
switch to `sweep`. If the log shows neither, the device isn't on a supported interface.

## Build & install

```sh
./build.sh          # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
```
Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. Then set `nds_mode:sweep` in
`.adds/nickeldissolve/config` and reboot to enable the animation.

## Uninstall
Delete `.adds/nickeldissolve/uninstall` and reboot.

## Credits

Built with support from large language models, and with prior Kobo modding experience from:

- [NickelHome](https://github.com/nicoverbruggen/NickelHome)
- [NickelTypeFix](https://github.com/nicoverbruggen/NickelTypeFix)
- [NickelCoverFix](https://github.com/nicoverbruggen/NickelCoverFix)
