# NickelDissolve

A [NickelHook](https://github.com/pgaskin/NickelHook) mod that gives Kobo a **Kindle-style
directional page-turn animation** — the new page sweeps in as a band-wipe across the screen,
instead of a single flat refresh.

> **Status: working prototype on the Kobo Clara B&W.** The wipe, greyscale quality, and
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
| `nds_log_ioctl` | 1 | log the hwtcon ioctl stream / sweep events |

Changes take effect on reboot (config is read once at startup). Tuning guide: leave
`nds_strip_waveform:0` (keep) for best quality on any device; use `nds_strips`/`nds_delay_us`
for smoothness/speed.

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

## Platform support

The `hwtcon` (MTK) and `mxcfb` (i.MX) update paths are both in the platform table, so the mod
targets any of these — but only the Clara B&W is verified so far.

- **Clara B&W** (MTK/hwtcon, "Spa BW") — **working, tested.**
- **Clara Colour / Libra Colour** (MTK/hwtcon, "Spa Colour" / "Monza") — *should* work: same
  interface, and reusing the turn's own waveform preserves colour/CFA pages. **Untested** — run in
  `observe` first, then `sweep`.
- **Libra 2 & most pre-2024 Kobos** (i.MX/`mxcfb`) — the mxcfb path is wired up but **untested**;
  i.MX turns use `REAGL` (partial), which the turn-armed trigger handles. Try `observe` first.
- **Older AllWinner / other** Kobos — not in the table; the mod loads but is inert (safe).

## Build & install

```sh
./build.sh          # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
```
Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. Then set `nds_mode:sweep` in
`.adds/nickeldissolve/config` and reboot to enable the animation.

## Uninstall
Delete `.adds/nickeldissolve/uninstall` and reboot.
