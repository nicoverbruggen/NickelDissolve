# NickelDissolve: technical details

The technical companion to the [README](README.md): how the mod works, the full configuration
reference (including debug settings), per-platform defaults, device internals, the roadmap, and
build instructions.

A [NickelHook](https://github.com/pgaskin/NickelHook) mod. The Kindle's smooth page-turn wipe is
a native MediaTek hardware "swipe" the Kobo drivers don't expose, so NickelDissolve approximates
it from userspace: it intercepts the single full-screen e-ink update Nickel issues per page turn
and replaces it with a **sequence of partial updates over vertical strips, swept across the
screen**. The framebuffer already holds the new page, so each strip transitions that band from
the old page to the new one; sweeping the bands reads as a directional wipe. It's a serial
sequence of partial refreshes, so it's a **stepped** wipe, not a perfectly smooth hardware swipe.

## How it works

Three hooks, all resolved by symbol name (so no per-firmware offsets):

- **`ReadingView::nextPageWithTimer` / `prevPageWithTimer`** (in `libnickel`): fire on a page turn. They **arm the sweep** and record the direction (a backward turn sweeps the opposite way).
- **`ioctl`** (in `libkobo`): when a turn is armed, the next full-screen e-ink update is the page render; in `sweep` mode it's replaced with N swept partial-strip updates (except Kaleido *colour* pages, which are detected from their CFA flag bits and left to full-refresh normally; see `nds_debug_color_skip`). Each strip **reuses the turn's own waveform** (so whatever the device/page uses, whether GLR16, REAGL, or a Kaleido CFA mode, is preserved). The **last strip reuses Nickel's update marker**, so its following `WAIT_FOR_UPDATE_COMPLETE` resolves and there's no hang; a fallback resubmits the original if that strip fails.
- **Kobo-agnostic.** The two flat Kobo e-ink interfaces (`hwtcon` on MTK, `mxcfb` on i.MX) share the same update-struct prefix, so the mod recognises either by its `ioctl` number and drives it by field offset. There's no per-struct code. The AllWinner **`sunxi`** interface (Elipsa/Sage) is different (a pointer-based `ubuffer` with a generic frame-sync wait), so it has its own small code path, but the same idea (chop the turn's full-screen update into swept strips). Screen dimensions are read from the update itself (resolution-independent).

## Configuration reference

**The config file is optional and none is shipped.** With no config, the mod runs on its
defaults; the wipe is on out of the box (except Elipsa/Sage, see below). To override, create a
plain-text `.adds/nickel-dissolve/config` with one `key:value` per line, containing only the keys
you want to change; delete it to return to the defaults. Changes take effect on reboot (config
is read once at startup). Legacy pre-rework key names are ignored with a rename hint in the log.

User settings (also documented on-device in `.adds/nickel-dissolve/doc`):

| key | default | meaning |
|---|---|---|
| `nds_mode` | `sweep` | `sweep` = do the wipe; `observe` = passthrough + log only; `off` = fully inert. **Exception:** sunxi (Elipsa/Sage) only sweeps when `nds_mode:sweep` is set explicitly; the default there behaves like `observe` |
| `nds_strips` | 8 | number of vertical bands (2–32). More = smoother + slower |
| `nds_delay_us` | *(auto, per-platform)* | extra pause between strips, µs (0–50000); the animation-speed dial. **Unset = per-platform default** (see below); set a value to override |
| `nds_direction` | `rtl` | forward-turn sweep direction (`rtl` or `ltr`); back turns flip automatically |
| `nds_animate_on_swipe` | 1 | `0` = swipe turns don't animate |
| `nds_animate_on_tap` | 1 | `0` = tap turns don't animate (the classic swipe-animates/tap-instant split; see *Per-gesture control* below) |
| `nds_animate_on_button` | 1 | `0` = physical page-turn button presses don't animate |

Debug settings (`nds_debug_*`), for troubleshooting and experiments, not everyday use; the
defaults are correct per device:

| key | default | meaning |
|---|---|---|
| `nds_debug_strip_waveform` | 0 = platform default | **0** keeps the turn's own waveform on hwtcon/mxcfb (best quality) and uses GL16 on sunxi (the turn's REAGL is too slow on those big panels). Non-zero forces a raw, **platform-specific** id (hwtcon: 1=DU 3=GL16 4=GLR16 6=A2; mxcfb: REAGL=6 GC16=2; sunxi: 0x20=GL16 0x40=GLR16) |
| `nds_debug_wait` | `submission` | between strips wait for `submission` (fast) or `complete` (slower, more discrete) |
| `nds_debug_cfa_skip` | 1 | **Kaleido colour panels:** set `HWTCON_FLAG_CFA_SKIP` on the swept strips (skips the per-region CFA colour pass whose boundaries seam). Correct because only B&W reading turns are ever swept; no-op on mono/i.MX |
| `nds_debug_force_bw` | 0 | **Kaleido colour panels:** `1` = force the **whole device** to greyscale by OR-ing `HWTCON_FLAG_CFA_SKIP` into *every* hwtcon update (menus, home, covers, reading). A full-B&W / accessibility switch, independent of the animation; also a quick way to confirm the panel's colour pipeline is the variable. No-op on mono/i.MX |
| `nds_debug_sweep_any_waveform` | 0 | `1` = bypass the greyscale-waveform allowlist and sweep *any* non-GC16 update (the old behaviour), including colour pages. For experiments/diagnosis; expect colour turns to look wrong |

Verbose page-turn / ioctl tracing is controlled by the standard **`nds_log`** key (default `0`), not a `nds_debug_*` key: set `nds_log:1` to log the ioctl stream and sweep events (each SEND logs `wf=<id>(<name>) dither=0x.. flags=0x..`), or use `nds_mode:observe` to log without animating. A supported device in `sweep` mode with a clean config stays quiet: only the startup block (mod version, firmware version, resolved settings). A malformed config turns tracing on for that boot automatically so mistakes self-diagnose. (The former `nds_debug_log_ioctl` key, which logged by default, has been replaced by `nds_log`.)

Tuning guide: leave `nds_debug_strip_waveform:0` for best quality; use `nds_strips`/`nds_delay_us` for smoothness/speed.

### Per-platform defaults

The three e-ink platforms need different pacing, so unset knobs fall back to a per-platform default (an explicit value in the config always wins). This is why the wipe looks right on each device out of the box:

| Platform | `nds_delay_us` (unset) | `nds_debug_strip_waveform` (0) | why |
|---|---|---|---|
| hwtcon (Clara BW/Colour, Libra Colour) | 0 | keep the turn's (GLR16) | the driver's submission-wait already paces the strips |
| mxcfb (Libra 2, Clara 2E) | 20000 | keep the turn's (REAGL) | no submission-wait, so strips fire near-instantly without a delay |
| sunxi (Elipsa, Sage) | 0 | GL16 | huge panel: the turn's REAGL is too slow, so a faster strip waveform matters more than added delay |

**Why mxcfb keeps REAGL rather than switching to GL16 like sunxi:** on these smaller i.MX panels REAGL is already fast; the fix there is to *slow the sweep down* with `nds_delay_us`, not speed the waveform up. GL16 is a sunxi-only workaround for the large-panel REAGL slowness. You can still force GL16 on mxcfb via `nds_debug_strip_waveform` if you want to experiment, but it isn't the default on purpose.

## Per-gesture control

By default **every page-turn gesture animates** (swipes, taps, and physical page-turn buttons), and each can be disabled individually via `nds_animate_on_swipe` / `nds_animate_on_tap` / `nds_animate_on_button`.

Telling the gestures apart takes two cooperating signals, because the hooks alone can't do it: swipes *and* button presses both arrive via the hookable `ReadingView::nextPageWithTimer` / `prevPageWithTimer` pair, while a tap-to-turn goes through `nextPage` / `prevPage`, which are not PLT-hookable (`ABS32`-only). So the mod also installs an **app-wide Qt event filter** (`gesture.cc`) that classifies the most recent finished input: a key press is a BUTTON, a press→release that moved less than ~40 px is a TAP (recording the tap position), anything longer is a SWIPE. The sweep decision then pairs the signals:

- **armed render + last input was a key** → button turn, gated by `nds_animate_on_button`;
- **armed render otherwise** → swipe turn, gated by `nds_animate_on_swipe`;
- **unarmed full-screen render + a fresh TAP** → tap turn, gated by `nds_animate_on_tap`. The tap's position supplies the direction (left half = backward, right half = forward, matching Kobo's reading tap zones), so tapped turns sweep the right way instead of reusing the last swipe's direction.

The filter observes only; every event passes through unchanged. If it isn't installed yet (no Qt application at plugin load; the page-turn hooks retry from the UI thread), tap detection falls back to the old behaviour: any full-screen page render animates while `nds_animate_on_tap` is enabled, using the last swipe's direction. Residual caveat even with the filter: a tap that triggers a non-turn full-screen render (rare on the reading screen; most such renders are GC16 flashes, which never animate) can still catch an animation.

## Safety internals

- In `sweep` (the default), **only the full-screen update immediately following a page turn is ever touched** (and the arming expires after ~2 s); all other ioctls pass through byte-for-byte, so menus/UI/images are unaffected.
- **`nds_mode:observe` changes nothing on screen:** it forwards every ioctl untouched and only logs; `nds_mode:off` is fully inert (no logging either).
- **No hang:** the last strip carries Nickel's marker; a hang-safety fallback resubmits the original update if that strip fails.
- Hook is `optional` (firmware mismatch = inert). Worst realistic failure is a garbled frame fixed by the next refresh: recoverable, not a brick.
- Revert with `nds_mode:off` (or `observe` to keep the logs), or delete `.adds/nickel-dissolve/uninstall` and reboot to remove the mod.

## Device support

**Officially supported: the modern MediaTek (`hwtcon`) devices.** These are the Kobo Clara BW, Clara Colour, and Libra Colour, and they are what the animation is built and tested for. The i.MX (`mxcfb`) and AllWinner (`sunxi`) code paths are still present, so the mod installs and can run on those devices too, but they are **not officially supported and may not work**; on them the Reading-settings entry shows an *Unsupported* note in place of the toggle. *Tested by author* = personally run on the hardware.

| Device | Chipset | Platform | Officially supported | Tested by author |
|---|---|---|---|---|
| **Clara BW** | MediaTek MT8113T | hwtcon (mono) | ✅ Yes | ✅ Yes |
| **Clara Colour** | MediaTek MT8113T | hwtcon (Kaleido) | ✅ Yes | ✅ Yes |
| **Libra Colour** | MediaTek MT8113T | hwtcon (Kaleido) | ✅ Yes | ✅ Yes |
| **Elipsa 2E** | MediaTek MT8113T | hwtcon (mono) | ✅ Yes (same modern family) | ❌ No |
| **Libra 2** | NXP i.MX6SLL | mxcfb | ❌ No (may work if installed) | ❌ No |
| **Clara 2E** | NXP i.MX6SLL | mxcfb | ❌ No (may work if installed) | ❌ No |
| **Elipsa** | AllWinner B300 | sunxi | ❌ No (may work if installed) | ✅ Yes |
| **Sage** | AllWinner B300 | sunxi | ❌ No (may work if installed) | ❌ No |

The "officially supported" test is the driver family: the mod treats the modern `hwtcon` interface as supported and everything else (`mxcfb`, `sunxi`, or an unrecognised platform) as unsupported. On an unrecognised platform the mod stays inert (no animation, no risk); it won't sweep unless the device is on one of the interfaces above.

The sunxi devices (Elipsa, Sage) use a **different update interface** (`DISP_EINK_UPDATE2`, a pointer-based `ubuffer`, generic frame-sync wait) that doesn't fit the flat offset table, so they get a dedicated code path. It works (a real directional wipe, confirmed on an Elipsa), but these are big panels, and each e-ink band renders slowly on them, so the sweep is more of a deliberate wipe than a quick one. It's fully functional, just **not recommended** at these display sizes, so unlike the other platforms it does not sweep by default: create a config with an explicit `nds_mode:sweep` if you want it anyway.

**Colour devices (Kaleido): how colour pages are skipped.** The strip-by-strip sweep can distort a *colour* page (the CFA colour conversion needs the whole frame), so colour pages must full-refresh instead of sweeping. The mod discriminates colour from B&W by the update's **waveform id**, not the CFA flag field. This was settled by reverse-engineering `libkobo` (Libra Colour, fw 4.45.23697):

- `KoboScreenMTK::handleAutoWfm(QRect, flags, hwtcon_update_data&)` resolves the waveform per render, calling `KoboScreenMTK::fbIsGray(QRect)`, Nickel's own per-region grayscale test. Grey content gets a **greyscale** waveform (`GLR16`=4 / `GL16`=3); non-grey content gets a **colour** waveform (`GCC16`=10 / `GLRC16`=11 / `GCK16`=8 / `GLKW16`=9). The id→name table is `KoboScreenMTK::idToWaveform`.
- The CFA colour-mode field (`HWTCON_FLAG_CFA_FLDS_MASK`/`0x7f00`) is a **device** property, not a content one: on-device logs show it set (mode G2, `flags=0x600`) on *every* reader update including B&W text, which is why the earlier field-based skip suppressed all animation.

So `nds_wf_sweepable` sweeps **only** `GL16`/`GLR16` (greyscale reading turns). That single test skips colour pages, `GC16` flashes, `AUTO`(257), and `A2`/`DU` menu updates, and because those are also the waveforms the home screen and menus use, it doubles as the reader-context guard (returning to the home screen never gets swept). On a mono panel (mxcfb, no CFA) there's nothing to avoid, so any non-flash waveform sweeps. Swept strips still set `HWTCON_FLAG_CFA_SKIP` to avoid per-region seams (`nds_debug_cfa_skip`); `nds_debug_sweep_any_waveform:1` bypasses the allowlist for experiments.

**Trying an untested device:** create a config with `nds_mode:observe` before the first reboot; it changes nothing and logs the ioctl stream. Confirm the log shows a platform (`[hwtcon]`/`[mxcfb]`) and `turn:` lines, then remove the line (or set `sweep`). If the log shows neither, the device isn't on a supported interface (the mod stays inert there anyway).

## Roadmap / TODO

This is a work in progress. Known gaps and things I still want to do, roughly in priority order:

- **✅ Waveform-based colour skip, confirmed on hardware (Libra Colour, fw 4.45.23697).** A colour kepub logged `SWEEP … wf=4(GLR16)` on every B&W text turn and `SKIP … wf=10(GCC16) … not a B&W reading turn` on every colour/image turn, with `flags=0x600` present on *both* (confirming the CFA field is device-level, not content). The `GL16`/`GLR16`-only allowlist separates them cleanly; menu (`AUTO`/`DU`) and exit-to-home (`GC16`) renders were also correctly passed through. Clara Colour shares the exact MT8113T hwtcon-Kaleido `KoboScreenMTK` path, so the same holds; Clara BW is mono (GLR16 turns, no colour). Remaining: physically confirm on the two Clara models.
- **✅ Per-gesture control, confirmed on hardware.** Startup logged `gesture_filter=1` and swipe turns animated with correct direction. Remaining: confirm tap direction-from-position and BUTTON classification on a device with page-turn buttons (the Libra Colour has none).
- **sunxi (Elipsa / Sage) speed.** The sunxi path works and produces a directional wipe, but each e-ink band is slow on these large panels, so it's not recommended there (see the table). Worth revisiting with waveform/strip tuning to see if it can be made snappy enough to recommend. Sage is untested.
- **Non-supported devices.** The i.MX (Libra 2, Clara 2E) and sunxi (Elipsa, Sage) code paths remain in the mod so those devices can still install and run it, but they are not officially supported and may not work; the settings entry marks them Unsupported. Elipsa 2E shares the modern `hwtcon` interface with the supported devices and is treated as supported, but hasn't been run on hardware.

## Build

```sh
./build.sh # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
```

Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. The animation is on after that
reboot, no configuration needed. (Elipsa/Sage are the exception: create a config with an
explicit `nds_mode:sweep` to opt in there.)
