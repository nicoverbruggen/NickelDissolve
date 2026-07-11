# NickelDissolve — technical details

The technical companion to the [README](README.md): how the mod works, the full configuration
reference (including debug settings), per-platform defaults, device internals, the roadmap, and
build instructions.

A [NickelHook](https://github.com/pgaskin/NickelHook) mod. The Kindle's smooth page-turn wipe is
a native MediaTek hardware "swipe" the Kobo drivers don't expose, so NickelDissolve approximates
it from userspace: it intercepts the single full-screen e-ink update Nickel issues per page turn
and replaces it with a **sequence of partial updates over vertical strips, swept across the
screen**. The framebuffer already holds the new page, so each strip transitions that band from
the old page to the new one — sweeping the bands reads as a directional wipe. It's a serial
sequence of partial refreshes, so it's a **stepped** wipe, not a perfectly smooth hardware swipe.

## How it works

Three hooks, all resolved by symbol name (so no per-firmware offsets):

- **`ReadingView::nextPageWithTimer` / `prevPageWithTimer`** (in `libnickel`) — fire on a page turn. They **arm the sweep** and record the direction (a backward turn sweeps the opposite way).
- **`ioctl`** (in `libkobo`) — when a turn is armed, the next full-screen e-ink update is the page render; in `sweep` mode it's replaced with N swept partial-strip updates (except Kaleido *colour* pages, which are detected from their CFA flag bits and left to full-refresh normally — see `nds_debug_color_skip`). Each strip **reuses the turn's own waveform** (so whatever the device/page uses — GLR16, REAGL, a Kaleido CFA mode — is preserved). The **last strip reuses Nickel's update marker**, so its following `WAIT_FOR_UPDATE_COMPLETE` resolves — no hang; a fallback resubmits the original if that strip fails.
- **Kobo-agnostic.** The two flat Kobo e-ink interfaces (`hwtcon` on MTK, `mxcfb` on i.MX) share the same update-struct prefix, so the mod recognises either by its `ioctl` number and drives it by field offset. There's no per-struct code. The AllWinner **`sunxi`** interface (Elipsa/Sage) is different — a pointer-based `ubuffer` with a generic frame-sync wait — so it has its own small code path, but the same idea (chop the turn's full-screen update into swept strips). Screen dimensions are read from the update itself (resolution-independent).

## Configuration reference

**The config file is optional and none is shipped.** With no config, the mod runs on its
defaults — the wipe is on out of the box (except Elipsa/Sage, see below). To override, create a
plain-text `.adds/nickel-dissolve/config` with one `key:value` per line, containing only the keys
you want to change; delete it to return to the defaults. Changes take effect on reboot (config
is read once at startup). Legacy pre-rework key names are ignored with a rename hint in the log.

User settings (also documented on-device in `.adds/nickel-dissolve/doc`):

| key | default | meaning |
|---|---|---|
| `nds_mode` | `sweep` | `sweep` = do the wipe; `observe` = passthrough + log only; `off` = fully inert. **Exception:** sunxi (Elipsa/Sage) only sweeps when `nds_mode:sweep` is set explicitly — the default there behaves like `observe` |
| `nds_strips` | 8 | number of vertical bands (2–32). More = smoother + slower |
| `nds_delay_us` | *(auto, per-platform)* | extra pause between strips, µs (0–50000) — the animation-speed dial. **Unset = per-platform default** (see below); set a value to override |
| `nds_direction` | `rtl` | forward-turn sweep direction (`rtl` or `ltr`); back turns flip automatically |
| `nds_tap_animates` | 0 | `0` = only **swipes** animate (a **tap** turns instantly); `1` = taps animate too (see *Swipe vs tap* below) |

Debug settings (`nds_debug_*`) — for troubleshooting and experiments, not everyday use; the
defaults are correct per device:

| key | default | meaning |
|---|---|---|
| `nds_debug_strip_waveform` | 0 = platform default | **0** keeps the turn's own waveform on hwtcon/mxcfb (best quality) and uses GL16 on sunxi (the turn's REAGL is too slow on those big panels). Non-zero forces a raw, **platform-specific** id (hwtcon: 1=DU 3=GL16 4=GLR16 6=A2; mxcfb: REAGL=6 GC16=2; sunxi: 0x20=GL16 0x40=GLR16) |
| `nds_debug_wait` | `submission` | between strips wait for `submission` (fast) or `complete` (slower, more discrete) |
| `nds_debug_cfa_skip` | 1 | **Kaleido colour panels:** skip the per-region CFA colour pass on the strips (removes the boundary seams). Correct for B&W; no-op on mono/i.MX |
| `nds_debug_color_skip` | 1 | **Kaleido colour panels:** don't sweep *colour* pages — a turn whose update carries a CFA colour mode (`flags & 0x7f00`) passes through and full-refreshes normally, since the whole-frame colour conversion can't be chopped into strips. `0` sweeps them anyway (colour may look wrong). No-op on mono/i.MX |
| `nds_debug_log_ioctl` | 1 | log the ioctl stream / sweep events |

Tuning guide: leave `nds_debug_strip_waveform:0` for best quality; use `nds_strips`/`nds_delay_us` for smoothness/speed.

### Per-platform defaults

The three e-ink platforms need different pacing, so unset knobs fall back to a per-platform default (an explicit value in the config always wins). This is why the wipe looks right on each device out of the box:

| Platform | `nds_delay_us` (unset) | `nds_debug_strip_waveform` (0) | why |
|---|---|---|---|
| hwtcon (Clara BW/Colour, Libra Colour) | 0 | keep the turn's (GLR16) | the driver's submission-wait already paces the strips |
| mxcfb (Libra 2, Clara 2E) | 20000 | keep the turn's (REAGL) | no submission-wait, so strips fire near-instantly without a delay |
| sunxi (Elipsa, Sage) | 0 | GL16 | huge panel: the turn's REAGL is too slow, so a faster strip waveform matters more than added delay |

**Why mxcfb keeps REAGL rather than switching to GL16 like sunxi:** on these smaller i.MX panels REAGL is already fast — the fix there is to *slow the sweep down* with `nds_delay_us`, not speed the waveform up. GL16 is a sunxi-only workaround for the large-panel REAGL slowness. You can still force GL16 on mxcfb via `nds_debug_strip_waveform` if you want to experiment, but it isn't the default on purpose.

## Swipe vs tap

By default, **the animation fires on a swipe but not on a tap** — a tapped page turn is instant. This is deliberate, and it falls out of how the sweep is triggered: the animation is armed by hooking `ReadingView::nextPageWithTimer` / `prevPageWithTimer`, which is the path a **swipe** gesture takes. A **tap**-to-turn goes through `nextPage` / `prevPage`, which are not PLT-hookable (`ABS32`-only), so a tap never arms the sweep. Many people like this split (a deliberate swipe animates; a quick tap stays instant), so it's the default.

Set **`nds_tap_animates:1`** to animate taps too. Caveat, because taps can't be hooked individually: in this mode the sweep is armed by **any** full-screen page render, so (a) it may occasionally animate a non-turn full-screen refresh, and (b) a tap has no direction signal, so tapped turns reuse the **last swipe's** direction (or the configured `nds_direction` if you haven't swiped yet). Recommended only if you turn primarily by tapping and don't mind those trade-offs.

## Safety internals

- In `sweep` (the default), **only the full-screen update immediately following a page turn is ever touched** (and the arming expires after ~2 s); all other ioctls pass through byte-for-byte, so menus/UI/images are unaffected.
- **`nds_mode:observe` changes nothing on screen** — it forwards every ioctl untouched and only logs; `nds_mode:off` is fully inert (no logging either).
- **No hang:** the last strip carries Nickel's marker; a hang-safety fallback resubmits the original update if that strip fails.
- Hook is `optional` (firmware mismatch = inert). Worst realistic failure is a garbled frame fixed by the next refresh — recoverable, not a brick.
- Revert with `nds_mode:off` (or `observe` to keep the logs), or delete `.adds/nickel-dissolve/uninstall` and reboot to remove the mod.

## Device support

Target coverage is **Elipsa and newer**. *Supported* = the mod drives this platform; *Tested by author* = personally run on the hardware (untested devices share an interface with a tested one, so they should work, but haven't been verified).

| Device | Chipset | Platform | Supported | Tested by author |
|---|---|---|---|---|
| **Clara BW** | MediaTek MT8113T | hwtcon (mono) | ✅ Yes | ✅ Yes |
| **Clara Colour** | MediaTek MT8113T | hwtcon (Kaleido) | ✅ Yes | ✅ Yes |
| **Libra Colour** | MediaTek MT8113T | hwtcon (Kaleido) | ✅ Yes | ✅ Yes |
| **Elipsa 2E** | MediaTek MT8113T | hwtcon (mono) | ✅ Yes | ❌ No |
| **Libra 2** | NXP i.MX6SLL | mxcfb | ✅ Yes | ❌ No |
| **Clara 2E** | NXP i.MX6SLL | mxcfb | ✅ Yes | ❌ No |
| **Elipsa** | AllWinner B300 | sunxi | ⚠️ Not recommended | ✅ Yes |
| **Sage** | AllWinner B300 | sunxi | ⚠️ Not recommended | ❌ No |

**Any device not listed above is unsupported by default.** On an unrecognised platform the mod simply stays inert (no animation, no risk) — it won't sweep unless the device is on one of the interfaces above.

The sunxi devices (Elipsa, Sage) use a **different update interface** (`DISP_EINK_UPDATE2`, a pointer-based `ubuffer`, generic frame-sync wait) that doesn't fit the flat offset table, so they get a dedicated code path. It works — a real directional wipe, confirmed on an Elipsa — but these are big panels, and each e-ink band renders slowly on them, so the sweep is more of a deliberate wipe than a quick one. It's fully functional, just **not recommended** at these display sizes, so unlike the other platforms it does not sweep by default: create a config with an explicit `nds_mode:sweep` if you want it anyway.

**Colour devices (Kaleido):** the strip-by-strip sweep would corrupt a *colour* page (the CFA colour conversion needs the whole frame), so only B&W/greyscale turns are animated. The mod detects colour pages from the update's CFA colour-mode flag bits (`HWTCON_FLAG_CFA_FLDS_MASK`, `0x7f00`) and skips the sweep on them — they full-refresh normally (`nds_debug_color_skip`, on by default). B&W/greyscale turns additionally set `HWTCON_FLAG_CFA_SKIP` on the strips to avoid per-region seams (`nds_debug_cfa_skip`).

**Trying an untested device:** create a config with `nds_mode:observe` before the first reboot — it changes nothing and logs the ioctl stream. Confirm the log shows a platform (`[hwtcon]`/`[mxcfb]`) and `turn:` lines, then remove the line (or set `sweep`). If the log shows neither, the device isn't on a supported interface (the mod stays inert there anyway).

## Roadmap / TODO

This is a work in progress. Known gaps and things I still want to do, roughly in priority order:

- **Verify the colour-page skip on hardware (Kaleido).** Implemented: the sweep now reads the update's CFA colour-mode flag bits (`flags & 0x7f00`) and passes colour pages through untouched, so they full-refresh normally (`nds_debug_color_skip`, default on) — this also covers turns onto mostly-image pages, which set the same CFA bits. Needs on-device confirmation on the Clara Colour / Libra Colour that Nickel sets the CFA field exactly on colour renders (`observe` mode logs `flags=`, and a skipped turn logs `SKIP colour page ... cfa=0x...`); if a B&W text page also carries a CFA mode, the detection needs to narrow.
- **sunxi (Elipsa / Sage) speed.** The sunxi path works and produces a directional wipe, but each e-ink band is slow on these large panels, so it's not recommended there (see the table). Worth revisiting with waveform/strip tuning to see if it can be made snappy enough to recommend. Sage is untested.
- **Per-gesture control (tap / swipe / button).** Today the split is swipe-animates / tap-instant (see *Swipe vs tap*), and on devices with page-turn buttons the buttons animate like swipes. I'd like explicit `nds_animate_on_tap` / `nds_animate_on_swipe` / `nds_animate_on_button` flags, which needs a Qt event filter to tell the gestures apart.
- **Untested devices.** Elipsa 2E, Clara 2E and Libra 2 share an interface with tested devices but haven't been run on hardware — see the table above.

## Build

```sh
./build.sh # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
```

Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. The animation is on after that
reboot — no configuration needed. (Elipsa/Sage are the exception: create a config with an
explicit `nds_mode:sweep` to opt in there.)
