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
directional wipe. No driver patching; only the driver's public `HWTCON_SEND_UPDATE` ioctl.

Because it's a serial sequence of partial refreshes (the driver's MDP coalesces rapid updates, so
each strip waits for submission), it is a **stepped** wipe, not the perfectly smooth hardware
swipe — but with enough strips and greyscale (GL16/GLR16) it reads well.

## How it works

Three hooks, all resolved by symbol name (so no per-firmware offsets):

- **`ioctl`** (in `libkobo`) — watches for the page-turn `HWTCON_SEND_UPDATE` (a full-screen
  `GLR16 FULL` update) and, in `sweep` mode, replaces it with N swept partial-strip updates. The
  **last strip reuses Nickel's original update marker**, so Nickel's following
  `WAIT_FOR_UPDATE_COMPLETE` resolves normally — no hang. If that strip ever fails to submit, it
  falls back to the original full update.
- **`ReadingView::nextPageWithTimer` / `prevPageWithTimer`** (in `libnickel`) — record the turn
  direction just before the update, so a **backward** turn sweeps the **opposite** way.

Screen dimensions are read from the intercepted update itself, so it is resolution-independent.

## Config (`.adds/nickeldissolve/config`)

| key | default | meaning |
|---|---|---|
| `nds_enabled` | 1 | master switch |
| `nds_mode` | `observe` | `observe` = passthrough + log (safe); `sweep` = do the wipe |
| `nds_strips` | 8 | number of vertical bands (2–32). More = smoother + slower |
| `nds_strip_waveform` | 1 | per-strip waveform: **1**=DU (fast, 2-level), **3**=GL16 (grey), **4**=GLR16 (Regal, best/slowest), 6=A2 |
| `nds_direction` | `rtl` | forward-turn sweep direction (`rtl` or `ltr`); back turns flip automatically |
| `nds_wait` | `submission` | between strips wait for `submission` (fast) or `complete` (slower, more discrete) |
| `nds_delay_us` | 0 | extra pause between strips, µs (0–50000) — the animation-speed dial |
| `nds_log_ioctl` | 1 | log the hwtcon ioctl stream / sweep events |

Changes take effect on reboot (config is read once at startup). Tuning guide: `nds_strip_waveform:3`
or `4` for antialiased text; `nds_strips`/`nds_delay_us` for smoothness/speed.

## Safety

- **`observe` (default) changes nothing** — it forwards every ioctl untouched and only logs.
- In `sweep`, **only a full-screen `GLR16 FULL` update is ever touched**; all other ioctls pass
  through byte-for-byte, so menus/UI/images are unaffected.
- **No hang:** the last strip carries Nickel's marker; a hang-safety fallback resubmits the
  original update if that strip fails.
- Hook is `optional` (firmware mismatch = inert). Worst realistic failure is a garbled frame
  fixed by the next refresh — recoverable, not a brick.
- Revert with `nds_mode:observe`, or delete `.adds/nickeldissolve/uninstall` and reboot.

## Platform support

- **Clara B&W** (MTK/hwtcon, "Spa BW") — **working, tested.**
- **Clara Colour / Libra Colour** (also MTK/hwtcon, "Spa Colour" / "Monza") — should run
  unchanged for B&W book turns; Kaleido *colour* pages may use a different waveform (gate won't
  match → no wipe, safe) or need CFA handling. Run in `observe` first to confirm.
- **Libra 2 and most pre-2024 Kobos** (i.MX/NXP `mxcfb`, or AllWinner) — **different platform.**
  The mod loads but is **inert** (its hwtcon ioctl is never issued). A port would need the mxcfb
  `MXCFB_SEND_UPDATE` path (different ioctl/struct/waveform ids).

## Build & install

```sh
./build.sh          # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
```
Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. Then set `nds_mode:sweep` in
`.adds/nickeldissolve/config` and reboot to enable the animation.

## Uninstall
Delete `.adds/nickeldissolve/uninstall` and reboot.
