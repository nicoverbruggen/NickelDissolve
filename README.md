# NickelDissolve

> [!WARNING]
> **Work in progress — not ready for general use.** This is an experimental prototype under active development. Only a few devices are verified, colour-page handling is incomplete, and the config and behaviour may still change. Try it only if you know what you're doing and don't mind rough edges. (It's reversible and shouldn't brick your device — see [Safety](#safety) — but there are no guarantees.)

A [NickelHook](https://github.com/pgaskin/NickelHook) mod that gives Kobo a **Kindle-style directional page-turn animation** — the new page sweeps in as a band-wipe across the screen, instead of a single flat refresh.

> **Status: working prototype on various devices.** The wipe, greyscale quality, and automatic forward/back direction all work on-device. See *Platform support* for other models.

## What it does

The Kindle's smooth page-turn wipe looks pretty cool, and I was wondering if this was possible on Kobo devices, too, as the same screen technology is used (either Carta or Kaleido E Ink).

NickelDissolve approximates it from userspace: it intercepts the single full-screen e-ink update Nickel issues per page turn and replaces it with a **sequence of partial updates over vertical strips, swept across the screen**. The framebuffer already holds the new page, so each strip transitions that band from the old page to the new one, sweeping the bands reads as a directional wipe.

It's a serial sequence of partial refreshes, so it's a **stepped** wipe, not a perfectly smooth hardware swipe, but with enough strips it looks pretty good!

## How it works

Three hooks, all resolved by symbol name (so no per-firmware offsets):

- **`ReadingView::nextPageWithTimer` / `prevPageWithTimer`** (in `libnickel`) — fire on a page turn. They **arm the sweep** and record the direction (a backward turn sweeps the opposite way).
- **`ioctl`** (in `libkobo`) — when a turn is armed, the next full-screen e-ink update is the page render; in `sweep` mode it's replaced with N swept partial-strip updates. Each strip **reuses the turn's own waveform** (so whatever the device/page uses — GLR16, REAGL, a Kaleido CFA mode — is preserved). The **last strip reuses Nickel's update marker**, so its following `WAIT_FOR_UPDATE_COMPLETE` resolves — no hang; a fallback resubmits the original if that strip fails.
- **Kobo-agnostic.** The two flat Kobo e-ink interfaces (`hwtcon` on MTK, `mxcfb` on i.MX) share the same update-struct prefix, so the mod recognises either by its `ioctl` number and drives it by field offset. There's no per-struct code. The AllWinner **`sunxi`** interface (Elipsa/Sage) is different — a pointer-based `ubuffer` with a generic frame-sync wait — so it has its own small code path, but the same idea (chop the turn's full-screen update into swept strips). Screen dimensions are read from the update itself (resolution-independent).

## Config

It's possible to configure the mod to behave differently. Here's what you can customize:

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

Changes take effect on reboot (config is read once at startup). Tuning guide: leave `nds_strip_waveform:0` (keep) for best quality on any device; use `nds_strips`/`nds_delay_us` for smoothness/speed.

## Swipe vs tap

By default, **the animation fires on a swipe but not on a tap** — a tapped page turn is instant. This is deliberate, and it falls out of how the sweep is triggered: the animation is armed by hooking `ReadingView::nextPageWithTimer` / `prevPageWithTimer`, which is the path a **swipe** gesture takes. A **tap**-to-turn goes through `nextPage` / `prevPage`, which are not PLT-hookable (`ABS32`-only), so a tap never arms the sweep. Many people like this split (a deliberate swipe animates; a quick tap stays instant), so it's the default.

Set **`nds_tap_animates:1`** to animate taps too. Caveat, because taps can't be hooked individually: in this mode the sweep is armed by **any** full-screen page render, so (a) it may occasionally animate a non-turn full-screen refresh, and (b) a tap has no direction signal, so tapped turns reuse the **last swipe's** direction (or the configured `nds_direction` if you haven't swiped yet). Recommended only if you turn primarily by tapping and don't mind those trade-offs.

## Safety

- **`observe` (default) changes nothing** — it forwards every ioctl untouched and only logs.
- In `sweep`, **only the full-screen update immediately following a page turn is ever touched** (and the arming expires after ~2 s); all other ioctls pass through byte-for-byte, so menus/UI/images are unaffected.
- **No hang:** the last strip carries Nickel's marker; a hang-safety fallback resubmits the original update if that strip fails.
- Hook is `optional` (firmware mismatch = inert). Worst realistic failure is a garbled frame fixed by the next refresh — recoverable, not a brick.
- Revert with `nds_mode:observe` (logs only), or delete `.adds/nickeldissolve/uninstall` and reboot.

## Device support

The following devices are supported:

| Device | SoC / interface | Status |
|---|---|---|
| **Clara BW** (Spa BW) | MTK / hwtcon (mono) | ✅ **Tested** — flawless |
| **Clara Colour** (Spa Colour) | MTK / hwtcon (Kaleido) | ✅ **Tested** (`nds_cfa_skip`, default on) |
| **Libra Colour** (Monza) | MTK / hwtcon (Kaleido) | ✅ **Tested** (`nds_cfa_skip`, default on) |
| **Elipsa 2E** (Condor) | MTK / hwtcon (mono) | 🟡 Same interface — ready, **untested** |
| **Libra 2** (Io) | i.MX / mxcfb (mx6sll) | 🟡 In the platform table — ready, **untested** |
| **Clara 2E** (Goldfinch) | i.MX / mxcfb | 🟡 In the platform table — ready, **untested** |
| **Elipsa** (Europa) | **AllWinner / sunxi** (B300) | ⚠️ **Works — not recommended** (large panel makes the wipe slow) |
| **Sage** (Cadmus) | **AllWinner / sunxi** (B300) | ⚠️ **Works** (same sunxi path) **— not recommended** (large panel); untested |

The sunxi devices (Elipsa, Sage) use a **different update interface** (`DISP_EINK_UPDATE2`, a pointer-based `ubuffer`, generic frame-sync wait) that doesn't fit the flat offset table, so they get a dedicated code path. It works — a real directional wipe, confirmed on an Elipsa — but these are big panels, and each e-ink band renders slowly on them, so the sweep is more of a deliberate wipe than a quick one. It's fully functional, just **not recommended** at these display sizes; enable `sweep` if you want it anyway. Inert (`observe`) by default.

**Colour devices (Kaleido):** the strip-by-strip sweep would corrupt a *colour* page (the CFA colour conversion needs the whole frame), so only B&W/greyscale turns are animated cleanly; see `nds_cfa_skip`. **Colour-page detection/skip is still a TODO.**

**Trying an untested device:** install in `nds_mode:observe` first — it changes nothing and logs the ioctl stream. Confirm the log shows a platform (`[hwtcon]`/`[mxcfb]`) and `turn:` lines, then switch to `sweep`. If the log shows neither, the device isn't on a supported interface.

## Roadmap / TODO

This is a work in progress. Known gaps and things I still want to do, roughly in priority order:

- **Colour-page detection/skip (Kaleido).** A *colour* page can't be swept — the CFA colour conversion needs the whole frame, so chopping it into strips corrupts the colour. Right now only B&W/greyscale turns come out clean. The plan is to read the update's CFA-mode flag bits and skip the sweep on colour pages (letting them full-refresh normally). Until then, colour pages may look wrong under `sweep`.
- **Image transitions on colour devices.** Turning *onto* a page that's mostly image on a Kaleido panel has the same whole-frame problem; folded into the colour-skip work above.
- **sunxi (Elipsa / Sage) speed.** The sunxi path works and produces a directional wipe, but each e-ink band is slow on these large panels, so it's not recommended there (see the table). Worth revisiting with waveform/strip tuning to see if it can be made snappy enough to recommend. Sage is untested.
- **Per-gesture control (tap / swipe / button).** Today the split is swipe-animates / tap-instant (see *Swipe vs tap*), and on devices with page-turn buttons the buttons animate like swipes. I'd like explicit `nds_animate_on_tap` / `nds_animate_on_swipe` / `nds_animate_on_button` flags, which needs a Qt event filter to tell the gestures apart.
- **Untested devices.** Elipsa 2E, Clara 2E and Libra 2 share an interface with tested devices but haven't been run on hardware — see the table above.

## Build & install

```sh
./build.sh # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
```
Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. 

Then set `nds_mode:sweep` in `.adds/nickeldissolve/config` and reboot to enable the animation.

## Uninstall

Delete `.adds/nickeldissolve/uninstall` and reboot.

## Credits

Built with support from large language models, and with prior Kobo modding experience from:

- [NickelHome](https://github.com/nicoverbruggen/NickelHome)
- [NickelTypeFix](https://github.com/nicoverbruggen/NickelTypeFix)
- [NickelCoverFix](https://github.com/nicoverbruggen/NickelCoverFix)
