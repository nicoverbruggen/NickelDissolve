# NickelDissolve: technical details

The technical companion to the [README](README.md): how the mod works, the full configuration
reference (including debug settings), per-platform defaults, device internals, a map of the
display hardware in every supported Kobo e-reader, the roadmap, and build instructions.

A [NickelHook](https://github.com/pgaskin/NickelHook) mod. The Kindle's smooth page-turn wipe is
a native MediaTek hardware "swipe" the Kobo drivers don't expose, so NickelDissolve approximates
it from userspace: it intercepts the single full-screen e-ink update Nickel issues per page turn
and replaces it with a **sequence of partial updates over vertical strips, swept across the
screen**. The framebuffer already holds the new page, so each strip transitions that band from
the old page to the new one; sweeping the bands reads as a directional wipe. It's a serial
sequence of partial refreshes, so it's a **stepped** wipe, not a perfectly smooth hardware swipe.

## How it works

Three hooks, all resolved by symbol name (so no per-firmware offsets):

- **`ReadingView::goToNextPage` / `goToPrevPage`** (in `libnickel`): the single sink every sequential page turn funnels into — a tap (`tapGesture` → `nextPage`), a swipe, and a physical button (`nextPageWithTimer`) all reach it through the PLT. They **arm the sweep** and record the **true direction** (which of the two fired; a backward turn sweeps the opposite way). Because it's the common sink, direction is Nickel's own decision rather than inferred, and non-turn renders (menus, book-open) never reach it, so they never arm.
- **`ioctl`** (in `libkobo`): when a turn is armed, the next full-screen e-ink update is the page render; in `sweep` mode it's replaced with **N swept partial-strip updates** (except colour pages on Kaleido panels, which are detected from the update's waveform and left to full-refresh normally). The band count and band waveform are tuned per device (see *Per-device defaults*): a mono panel's strips reuse the turn's own greyscale waveform (GLR16/REAGL, or the dark reading waveform in Dark Mode), while a Kaleido colour panel's strips use `glkw16`, a sharper Kaleido B&W mode, or its dark counterpart in Dark Mode. The **last strip reuses Nickel's update marker**, so its following `WAIT_FOR_UPDATE_COMPLETE` resolves and there's no hang; a fallback resubmits the original if that strip fails.
- **One driver per interface.** Each e-ink interface has its own source file: `driver_hwtcon.cc` (MediaTek), `driver_mxcfb.cc` (NXP i.MX, all three generations of its update struct) and `driver_sunxi.cc` (AllWinner). A driver is selected by the `ioctl` number, which encodes the size of its own argument, so a match also confirms the layout. `driver_common.h` holds only the descriptor the core needs to talk about any of them; the drivers share no logic, so they can diverge as more hardware is understood. They already have: the waveform ids collide between families (id 8 is `GCK16` on MediaTek but `DU4` on i.MX), which is exactly the kind of mistake separate files make hard to write. The AllWinner interface of the Elipsa and Sage does not share the flat update struct at all: its geometry sits behind a pointer and its flush mode is a separate word, which is why it is a driver rather than another row. Screen dimensions are read from the update itself (resolution-independent).
- **Device identification, no model list.** The two per-device dials are derived, not looked up. **Band count** follows the panel's *physical* width, from its pixel size and the density Qt reports, so devices of the same physical size tune identically whatever their resolution, with nothing to maintain. The **colour-vs-mono** choice for the band waveform comes from `Device::getCurrentDevice()->hasColorDisplay()` in `libnickel`, resolved by symbol; if that symbol is ever missing the mod assumes mono. No firmware version, model id, or serial number is read from disk.

## How e-ink refreshes work

Background for the above: a NickelDissolve strip, and every Kobo screen update, is one e-ink *refresh*, and a few properties of those refreshes shape the whole design.

**A pixel changes by moving particles, not by setting a value.** Each e-ink pixel is a microcapsule of charged black/white particles (plus coloured ones on Kaleido). Changing it means driving the panel through a *sequence of voltage pulses over many frames* so the particles physically migrate. That pulse sequence is the **waveform**.

**Refresh time is dominated by the waveform's frame count, not the region size.** The controller runs the waveform for its fixed duration whether the update covers the whole screen or a sliver; a bigger region costs more only in data/DMA, a secondary term. Splitting a turn into N strips therefore costs on the order of N refreshes, but what that costs in wall-clock time depends on the interface rather than the panel. Measured on-device, a band takes about 10 ms on MediaTek, which hands each one to the controller and moves on, against about 28 ms on i.MX, where there is no submission wait and the delay between strips is essentially the whole cost. `nds_strips` and `nds_delay_us` are the smoothness/speed dials.

**The waveform mode is the quality/speed dial**, and it's the vocabulary in the config and the logs:

- **GC16** — full 16-level greyscale, the "flash": it cycles pixels through black and white to fully settle, so no ghosting, but it visibly blinks and is slow. Kobo forces it on book-open, chapter jumps, and wake; NickelDissolve never sweeps a GC16 update, it lets that flash happen.
- **GLR16 / GL16 / REAGL** — flashless greyscale ("regal"): straight to the new image, no black blink, at a slight ghosting risk. This is the normal reading page-turn waveform, and what the swept strips reuse.
- **DU / A2** — 1–2 level, very fast and ghosty; used for menus and quick feedback.

**One update is one `ioctl` to the EPD controller**, describing a rectangle (top/left/width/height), a waveform mode, full vs partial, and a **marker** (an id). The controller reads the framebuffer for that rectangle and drives the panel through the waveform. Software then waits on the marker — either for *submission* (queued) or *completion* (settled on glass). That is how Nickel serialises a turn, and how the mod's last strip reuses Nickel's marker so the following wait resolves and nothing hangs.

**The panel must be powered to drive anything.** After a short idle the controller powers down the panel's source/gate drivers and the EPD PMIC (VCOM); the next update has to wake them first, and after a full system sleep the whole display pipeline re-inits. That wake latency falls on the first update after an idle, so the first turn after a pause or a wake is heavier than a warm one.

## Configuration reference

**The config file is optional and none is shipped.** With no config, the mod runs on its
defaults; the wipe is on out of the box. To override, create a
plain-text `.adds/nickel-dissolve/config` with one `key:value` per line, containing only the keys
you want to change; delete it to return to the defaults. Changes take effect on reboot (config
is read once at startup). Legacy pre-rework key names are ignored with a rename hint in the log.

User settings (also documented on-device in `.adds/nickel-dissolve/doc`):

| key | default | meaning |
|---|---|---|
| `nds_mode` | `sweep` | `sweep` = do the wipe; `observe` = passthrough + log only; `off` = fully inert |
| `nds_strips` | *(auto, by physical panel width)* | number of vertical bands (2–32). More = smoother + slower. **Unset = per-device default**, scaled so a band covers ~0.35 in on any screen and capped at 12 (10 on a Clara, 12 on a Libra Colour, 9 on an Elipsa; see below); set a value to override, up to 32 |
| `nds_delay_us` | *(auto, per-platform)* | extra pause between strips, µs (0–50000); the animation-speed dial. **Unset = per-platform default** (see below); set a value to override |
| `nds_direction` | `rtl` | forward-turn sweep direction (`rtl` or `ltr`); back turns flip automatically |
| `nds_animate_on_swipe` | 1 | `0` = swipe turns don't animate |
| `nds_animate_on_tap` | 1 | `0` = tap turns don't animate (the classic swipe-animates/tap-instant split; see *Per-gesture control* below) |
| `nds_animate_on_button` | 1 | `0` = physical page-turn button presses don't animate |

Debug settings (`nds_debug_*`), for troubleshooting and experiments, not everyday use; the
defaults are correct per device:

| key | default | meaning |
|---|---|---|
| `nds_debug_strip_waveform` | 0 = per-device default | **0** uses the per-device band waveform (colour panels: `glkw16` in day mode, `GLKW16` in Dark Mode; else reuse the turn's own waveform; see below). Non-zero forces a raw, **platform-specific** id (hwtcon: 1=DU 3=GL16 4=GLR16 8=glkw16 9=GLKW16; mxcfb: REAGL=6 GC16=2; sunxi flush modes: 0x402=DU 0x420=flashless regional, the default, 0x404=GC16) |
| `nds_debug_wait` | `submission` | between strips wait for `submission` (fast) or `complete` (slower, more discrete) |
| `nds_debug_cfa_skip` | 1 | **Kaleido colour panels:** set `HWTCON_FLAG_CFA_SKIP` on the swept strips (skips the per-region CFA colour pass whose boundaries seam). Correct because only B&W reading turns are ever swept; no-op on mono/i.MX |
| `nds_debug_force_bw` | 0 | **Kaleido colour panels:** `1` = force the **whole device** to greyscale by OR-ing `HWTCON_FLAG_CFA_SKIP` into *every* hwtcon update (menus, home, covers, reading). A full-B&W / accessibility switch, independent of the animation; also a quick way to confirm the panel's colour pipeline is the variable. No-op on mono/i.MX |
| `nds_debug_sweep_any_waveform` | 0 | `1` = bypass the greyscale-waveform allowlist and sweep *any* non-GC16 update (the old behaviour), including colour pages. For experiments/diagnosis; expect colour turns to look wrong |

Verbose page-turn / ioctl tracing is controlled by the standard **`nds_log`** key (default `0`), not a `nds_debug_*` key: set `nds_log:1` to log the ioctl stream and sweep events (each SEND logs `wf=<id>(<name>) dither=0x.. flags=0x..`), or use `nds_mode:observe` to log without animating. A supported device in `sweep` mode with a clean config stays quiet: only the startup block (mod version, firmware version, resolved settings). A malformed config turns tracing on for that boot automatically so mistakes self-diagnose. (The former `nds_debug_log_ioctl` key, which logged by default, has been replaced by `nds_log`.)

Tuning guide: leave `nds_debug_strip_waveform:0` for best quality; use `nds_strips`/`nds_delay_us` for smoothness/speed.

### Per-device defaults

Unset knobs fall back to a default derived from the hardware, so the wipe looks right on each device out of the box (an explicit value in the config always wins). Three things are tuned.

**Band count — by physical panel width.** The wider the screen *in inches*, the more bands, so a band covers about the same physical distance on every device and the wipe reads at a comparable pace. The target is **0.354 inches per band**, which is what the two tuned devices already do: a Clara is 1072 px at 300 dpi (3.57 in) with 10 bands, a Libra Colour is 1264 px at 300 dpi (4.21 in) with 12.

Pixel width is not a usable substitute. Both tuned devices are 300 dpi, so pixels and inches agree there and the difference is invisible; it stops being invisible at any other density. Counting pixels would give the 8-inch Sage more bands than the 10.3-inch Elipsa, because the Sage has more of them on a smaller screen. Density comes from Qt's `QScreen`, computed from the diagonal so rotation does not matter and bounded to 100–400 dpi; if it is unavailable the mod falls back to a flat 105 px per band.

| Device | Panel | Density | Physical width | `nds_strips` (unset) |
|---|---|---|---|---|
| Clara BW, Clara Colour, Clara 2E, Clara HD | 1072x1448 | 300 dpi | 3.57 in | 10 |
| Nia | 758x1024 | 212 dpi | 3.58 in | 10 |
| Aura HD | 1080x1440 | 265 dpi | 4.08 in | 12 |
| Libra Colour, Libra 2, Libra H2O | 1264x1680 | 300 dpi | 4.21 in | 12 |
| Aura ONE | 1404x1872 | 300 dpi | 4.68 in | 12 *(capped)* |
| Forma | 1440x1920 | 300 dpi | 4.80 in | 12 *(capped)* |
| Elipsa 2E | 1404x1872 | 227 dpi | 6.19 in | 12 *(capped)* |
| Sage | 1440x1920 | 300 dpi | 4.80 in | 7 *(AllWinner)* |
| Elipsa | 1404x1872 | 227 dpi | 6.19 in | 9 *(AllWinner)* |

**The derived count is capped at 12** (`NDS_MAX_AUTO_STRIPS`), which is what the old panel-size rule topped out at, so this is a ceiling that was always there implicitly. It is a *time* limit rather than a size one: a band does not cost the same everywhere. Measured on-device, a band takes about **10 ms** on the MediaTek interface, which hands each one off and moves on, against about **28 ms** on i.MX, which has no submission wait and paces with a delay instead. Past a point more bands only makes a turn slower without looking better. An explicit `nds_strips` still reaches 32.

**AllWinner devices use a wider band** (0.69 in against 0.354 in), because each band there is a full completion-waited refresh on an older controller driving a large panel. That figure is set from what reads well on an Elipsa rather than derived.

The Nia and the Clara are the clearest case for the rule: very different pixel counts, nearly identical physical width, so they get the same number of bands. The Aura ONE and the Elipsa are the mirror image, sharing a resolution at different densities: the Elipsa is physically much wider, and only the density tells them apart.

**Band waveform — by panel type, and day vs Dark Mode.** A Kaleido *colour* panel renders crisper, slightly faster B&W bands with `glkw16` (a Kaleido B&W-optimised waveform) in the reader's day mode. In Dark Mode the turn itself is the dark reading waveform `GLKW16`, and the bands follow it so they stay sharp and don't ghost. Mono panels reuse the turn's own greyscale waveform in either mode. The colour-vs-mono answer comes from `Device::hasColorDisplay()` (see *How it works*):

| Panel | Example devices | Band waveform, day | Band waveform, Dark Mode |
|---|---|---|---|
| Kaleido colour (hwtcon) | Libra Colour, Clara Colour | `glkw16` (raw id 8) | `GLKW16` (raw id 9) |
| mono (hwtcon) | Clara BW | reuse the turn's `GLR16` | reuse the turn's `GLKW16` |
| mono (mxcfb) | Libra 2, Clara 2E, Clara HD, Forma, Nia, Libra H2O | reuse the turn's `REAGL` | reuse the turn's waveform |
| mono (sunxi) | Elipsa, Sage | `0x420`, a flashless regional mode | same |

The day-mode `glkw16` is the raw *kernel* EPD id `8` the mod writes into the bands. libkobo's `KoboScreenMTK` numbers waveforms differently (it calls id 8 `GCK16` and glkw16 `9`), so the kernel id and the libkobo name can look inconsistent; the kernel is the authority for what actually runs, and id 8 was confirmed by eye to be the sharp Kaleido B&W mode on the Libra Colour and Clara Colour. In Dark Mode the mod instead writes kernel id `9` (`GLKW16`), matching the dark reading waveform the turn already uses; drawing the bands with the day id there ghosts badly, confirmed on a Clara Colour. On a mono panel the strips reuse the turn's own waveform, which keeps every band a full-quality render and matches whatever the panel supports.

**Inter-strip delay — by platform.** The three e-ink platforms pace themselves differently:

| Platform | `nds_delay_us` (unset) | why |
|---|---|---|
| hwtcon (MediaTek) | 0 | the driver's submission-wait already paces the strips |
| mxcfb (i.MX) | 30000 | no submission-wait, so strips need an explicit delay or the controller merges them into one instant refresh |
| sunxi (AllWinner) | 0 | each band is waited on by its own frame id, which paces it; without that wait the driver drops bands |

To tune the feel, reach for `nds_strips` (smoothness) and `nds_delay_us` (speed) first; `nds_debug_strip_waveform` can still force a specific waveform to experiment.

## Per-gesture control

By default **every page-turn gesture animates** (swipes, taps, and physical page-turn buttons), and each can be disabled individually via `nds_animate_on_swipe` / `nds_animate_on_tap` / `nds_animate_on_button`.

Direction and whether a turn even happened both come from the `goToNextPage` / `goToPrevPage` sink, so the mod infers neither. What the sink can't say is which input caused the turn, and the per-gesture toggles need that. So the mod also installs an **app-wide Qt event filter** (`gesture.cc`) that classifies the most recent finished input purely by type: a key press is a BUTTON, a press→release that moved less than ~40 px is a TAP, anything longer is a SWIPE. An armed turn then applies the matching toggle: a key → `nds_animate_on_button`, a tap → `nds_animate_on_tap`, otherwise (a swipe, or the filter not up yet) → `nds_animate_on_swipe`.

The filter observes only; every event passes through unchanged, and it never touches direction — that is always which of the two sinks fired. Because arming is the real turn sink, a center tap that merely raises the reading menu never reaches `goToNextPage`, so it never arms and never animates; there is no tap-zone or tap-position logic at all. (An earlier version predated the sink hook and inferred a tap's direction from which screen half it landed in, which sweeps the wrong way for readers who remap Kobo's tap zones; hooking the sink is what fixed that.)

## Safety internals

- In `sweep` (the default), **only the full-screen update immediately following a page turn is ever touched** (and the arming expires after ~2 s); all other ioctls pass through byte-for-byte, so menus/UI/images are unaffected.
- **`nds_mode:observe` changes nothing on screen:** it forwards every ioctl untouched and only logs; `nds_mode:off` is fully inert (no logging either).
- **No hang:** on the flat interfaces the last strip carries Nickel's marker, and a hang-safety fallback resubmits the original update if that strip fails. On AllWinner the marker is returned by the driver rather than chosen, so the last band's frame id is the one Nickel waits on by construction; if every band is refused, the original update is resubmitted.
- Hook is `optional` (firmware mismatch = inert). Worst realistic failure is a garbled frame fixed by the next refresh: recoverable, not a brick.
- Revert with `nds_mode:off` (or `observe` to keep the logs), or delete `.adds/nickel-dissolve/uninstall` and reboot to remove the mod.

## Kobo hardware map

A reference map of the display hardware in every Kobo e-reader that still receives firmware updates. The mod only runs on devices whose e-ink driver it recognises, so this table is what a decision to broaden support is made from. It is separate from *Device support* below, which records what the mod actually claims today.

Everything here was read out of Kobo's own firmware, not from community lists. The method is repeatable: Kobo builds one firmware image per **hardware platform** (`kobo3` through `kobo13`), not per device, and each firmware zip carries an `upgrade/` tree whose top directory names the SoC (`mx50-ntx`, `mx6sl-ntx`, `mx6sll-ntx`, `mx6ull-ntx`, `b300-ntx`, `mt8113t-ntx`) and whose subdirectories name the NTX board revisions. The device tree or kernel inside then names the EPD controller driver, which is what decides the ioctl interface. Firmware for all platforms was checked at 4.38.23684 / 4.38.23697 / 4.45.23697 (April and May 2026).

### Platforms, devices and e-ink interface

| Platform | SoC | EPD driver | Interface | Devices |
|---|---|---|---|---|
| `kobo3` | NXP i.MX50 | `mxc_epdc_fb` (Linux 2.6.35.3) | `mxcfb` | Touch A/B |
| `kobo4` | NXP i.MX50 | `mxc_epdc_fb` (Linux 2.6.35.3) | `mxcfb` | Touch C, Glo, Mini, Aura HD |
| `kobo5` | NXP i.MX50 | `mxc_epdc_fb` (Linux 2.6.35.3) | `mxcfb` | Aura, Aura H2O |
| `kobo6` | NXP i.MX6SL | `mxc_epdc_fb` (Linux 3.0.35) | `mxcfb` | Glo HD, Touch 2.0, Aura ONE, Aura H2O Edition 2 v1, Aura Edition 2 v1, Aura ONE Limited Edition |
| `kobo7` | NXP i.MX6SLL and i.MX6ULL | `fsl,imx6sll-epdc` / `fsl,imx7d-epdc` | `mxcfb` | Clara HD, Aura H2O Edition 2 v2, Aura Edition 2 v2, Forma, Nia (i.MX6ULL), Libra H2O |
| `kobo8` | AllWinner B300 | sunxi `EINK_*` | `sunxi` | Sage, Elipsa |
| `kobo9` | NXP i.MX6SLL | `fsl,imx6sll-epdc` / `fsl,imx7d-epdc` | `mxcfb` | Libra 2 |
| `kobo10` | NXP i.MX6SLL | `fsl,imx6sll-epdc` / `fsl,imx7d-epdc` | `mxcfb` | Clara 2E |
| `kobo11` | MediaTek MT8113T | `mediatek,hwtcon` | `hwtcon` | Elipsa 2E |
| `kobo12` | MediaTek MT8113T | `mediatek,hwtcon` | `hwtcon` | Clara BW (N365), Clara Colour |
| `kobo13` | MediaTek MT8113T | `mediatek,hwtcon` | `hwtcon` | Libra Colour |

The MediaTek device trees identify the SoC as `mediatek,mt8110` / `mediatek,mt8512`; `MT8113T` is the NTX platform name for the same part. The `hwtcon` block sits at `0x14000000` on all three MediaTek platforms.

### NTX board revisions

One device can ship in several board revisions, each with its own kernel and device tree, and the revisions are where the EPD panel and the EPD PMIC change. The mod cannot see the model name, so this is the level any hardware-dependent behaviour has to be reasoned about.

| Platform | Board revisions in the firmware | EPD PMIC (where the device tree says) |
|---|---|---|
| `kobo3` | `E60610` | not in a device tree (board data is compiled into the kernel) |
| `kobo4` | `E50610`, `E60610D`, `E606B0`, `E606C0` | as above |
| `kobo5` | `E606F0B`, `E606G0` | as above |
| `kobo6` | `E60Q90`, `E60QL0`, `E60QM0A`, `E60QM0B`, `E70Q00` | as above |
| `kobo7` | `E60K00`, `E60K00B0x00`, `E60QL0`, `E60QM0`, `E70K00`, `E80K00`, `E60U20A0x10`, `E60U20B0x00` | TI TPS6518x throughout, plus Silergy SY7636 on `E70K00` / `E80K00` / `E60U20`; `E60QL0` uses Fiti FP9928 instead |
| `kobo8` | `E80P00-A0x10`, `E80P00-C0x20`, `E80P00-D0x20`, `EA0P10-A0x10` | not exposed the same way (AllWinner `.fex` images) |
| `kobo9` | `E70K10`, `E70K10A0x10`, `E70K10B0x00`, `E70K10C0x00`, `E70K10E0x00`, `E70K10F0x00` | TPS6518x + SY7636; the **F revision adds Fiti JD9930** |
| `kobo10` | `E60K20`, `E60K20A0x10`, `E60K20B0x00` | TPS6518x + SY7636; the **B revision adds Fiti JD9930** |
| `kobo11` | `EA0T00-A0x30` | panel 1872x1404 |
| `kobo12` | `E60T00-D0x00`, `E60T00-E0x00` (each in an `-ON` and an `-OFF` variant) | panel 1448x1072 |
| `kobo13` | `E70T00-A0x00`, `E70T00-C0x00` | panel 1680x1264 |

The six Libra 2 board revisions are the clearest example of why this matters: they are one device to the user and to Nickel, but `E70K10F0x00` drives its panel through a different EPD PMIC than the rest. The MediaTek panel sizes are read straight from the `hwtcon` `epd` node and confirm which board code is which device.

### The three update interfaces

`libkobo.so` is a single binary that serves every device and picks its screen backend at run time. It is byte-identical across `kobo3` through `kobo11` on 4.38, and a second build serves `kobo12` / `kobo13` on 4.45. It carries three screen backends, each with a set of per-device configuration classes named by Kobo's internal codenames:

- `KoboScreenNXP` (`mxcfb`): Alyssum, Dahlia, Dragon, Frost, Goldfinch, Io, Kraken, Luna, Nova, Phoenix, Pika, Pixie, Snow, SnowSLL, Star, Storm, Trilogy
- `KoboScreenAllWinner` (`sunxi`): Cadmus, Dragon, Europa
- `KoboScreenMTK` (`hwtcon`): Condor, Monza, Spa

`libnickel` holds the table those codenames come from, as an array of `{codename, id, model name}`. Resolving its pointers (rather than reading strings in address order, which only shows what the linker put next to what) gives the mapping:

| Codename | Model | Codename | Model |
|---|---|---|---|
| `trilogy` | Touch | `nova` | Clara HD |
| `kraken` | Glo | `frost` | Forma |
| `pixie` | Mini | `luna` | Nia |
| `dragon` | Aura HD | `cadmus` | Sage |
| `phoenix` / `star` | Aura | `storm` | Libra H2O |
| `dahlia` | Aura H2O | `goldfinch` | Clara 2E |
| `alyssum` | Glo HD | `europa` | Elipsa |
| `pika` | Touch 2.0 | `io` | Libra 2 |
| `daylight` | Aura ONE | `condor` | Elipsa 2E |
| `snow` | Aura H2O Edition 2 | `monza` | Libra Colour |
| | | `spaBW` / `spaColour` | Clara BW / Clara Colour |

The three `KoboScreenMTK` configs line up exactly with the three MediaTek platforms: Condor is `kobo11`, Monza is `kobo13`, Spa is `kobo12`.

**A model can hold several device ids.** The table lists the Forma under both `377` and `380`, the Libra Colour under `390` and `394`, and the Clara BW under `391` and `395`. Firmware distribution uses only one id per model, which is why an id seen here may not appear in a firmware listing. Note also that the name stored here is not always the retail name: ids `375` and `379` are both stored as "Kobo Aura" but sell as the Aura Edition 2.

**Some retail models ship on two different SoCs.** These are the cases where one product name covers two platforms, which Kobo itself distinguishes by the 7th digit of the serial number:

| Model | id | Platform | SoC |
|---|---|---|---|
| Aura Edition 2 v1 | `375` | `kobo6` | i.MX6SL |
| Aura Edition 2 v2 | `379` | `kobo7` | i.MX6SLL |
| Aura H2O Edition 2 v1 | `374` | `kobo6` | i.MX6SL |
| Aura H2O Edition 2 v2 | `378` | `kobo7` | i.MX6SLL |
| Touch A/B | `310` | `kobo3` | i.MX50 |
| Touch C | `320` | `kobo4` | i.MX50 |

For the two Edition 2 models this crosses the boundary that decides whether the mod can run at all: `kobo6` takes the 68-byte update struct with the 4-byte wait, `kobo7` takes the 72-byte struct with the 8-byte wait. A model name therefore does not identify the display hardware, and neither does the SoC identify the waveform behaviour, since board revisions vary underneath both. This is why the mod detects the driver interface at run time instead of keeping a model list.

Decoding the `movw`/`movt` pairs that build the ioctl numbers in `libkobo.so` shows Kobo's userspace issues **three** different update commands. The struct size is encoded in the ioctl number itself, so it also states the layout each one expects:

| Command | ioctl | Update-struct size | Handled by the mod |
|---|---|---|---|
| `HWTCON_SEND_UPDATE` | `0x4024462E` | 36 bytes | yes |
| `MXCFB_SEND_UPDATE` (older) | `0x4044462E` | **68 bytes** | **no** |
| `MXCFB_SEND_UPDATE` (current) | `0x4048462E` | 72 bytes | yes |
| `*_WAIT_FOR_UPDATE_COMPLETE` | `0xC008462F` | 8 bytes | yes |
| `*_WAIT_FOR_UPDATE_COMPLETE` (older) | `0x4004462F` | 4 bytes | no |

The forms differ only in trailing fields, and the offsets the mod reads (`waveform_mode` at 16, `update_mode` at 20, `update_marker` at 24, `flags` at 32) are the same in all of them. So the only thing keeping the mod inert on an older i.MX device is that it does not match the ioctl number.

`libkobo` pairs those constants into two distinct NXP code paths, which the constants' positions in the binary make plain: the 68-byte send sits beside the 4-byte wait, and the 72-byte send sits directly beside the 8-byte wait. Unpacking every kernel (the i.MX6 ones are LZO-compressed inside the `zImage`) shows which platform answers to which:

| Platform | Kernel | `SEND_UPDATE` accepted | `WAIT_FOR_UPDATE_COMPLETE` | Mod matches |
|---|---|---|---|---|
| `kobo3`, `kobo4` | Linux 2.6.35.3 | 68 only | | no |
| `kobo5` | Linux 2.6.35.3 | **64 only** | | no |
| `kobo6` | Linux 3.0.35 | 64, 68 | 4-byte | no |
| `kobo7`, `kobo9`, `kobo10` | Linux 4.1.15 | 64, 68, **72** | 8-byte | **yes** |
| `kobo11`, `kobo12`, `kobo13` | Linux 4.9.77 | 36 (`hwtcon`) | 8-byte | **yes** |

The i.MX6 kernels keep legacy handlers for all three sizes, so only `kobo6` and older are genuinely restricted. In the i.MX50 kernels the constants sit in a contiguous four-entry dispatch table, which is what distinguishes them from a chance byte match: on `kobo3` that table also shows `SET_WAVEFORM_MODES` taking 24 bytes, and on `kobo5` it takes 40, meaning `kobo5` has more waveform slots. That is the same boundary REAGL appears at.

One inconsistency is unresolved: `kobo5` accepts only the 64-byte form, but `libkobo` contains no 64-byte constant. Either Nickel reaches that panel another way, or the 64-byte call is built somewhere this scan does not reach.

Two other capability signals come out of the kernels. `kobo3` and `kobo4` have **no REAGL waveform support at all**; it first appears in `kobo5` and is present in every platform after it. Dithering support first appears in `kobo6`. A band sweep depends on a flashless greyscale waveform, so the i.MX50 Touch, Glo, Mini and Aura HD are the least likely candidates for the animation regardless of the ioctl work.

### What the firmware does not answer

The firmware settles the *interface*: which ioctl to send, how big the struct is, and where the fields sit. It does not settle the *behaviour* the animation depends on.

- **Which waveform id a reading page turn actually uses on each i.MX device.** On MediaTek this is recoverable, because `libkobo` contains `KoboScreenMTK::idToWaveform` and `KoboScreenMTK::waveformModeFromQt`. The NXP backend has **no equivalent**: there is no `waveformModeFromQt`, no id-to-name table, and no waveform name strings anywhere in `libkobo`. The sweep gate (`nds_wf_sweepable`) is built on exactly this knowledge, so it cannot be derived for the older devices from firmware alone.
- **Waveform timing.** How long a waveform takes on a given panel decides the band count and `nds_delay_us`. Nothing in the firmware states it.
- **Whether a swept band looks right.** Panel and waveform-file behaviour, not code.
- **Why `kobo5` accepts only the 64-byte update struct when `libkobo` never builds that constant.** The ioctl sizes are settled for every other platform.

These are the values the existing devices were tuned from, and they were all obtained from on-device logs. Broadening support needs the same evidence from the older hardware, which is what `nds_mode:observe` exists to collect.

## Device support

**Officially supported: the modern MediaTek (`hwtcon`) devices.** These are the Kobo Clara BW, Clara Colour, and Libra Colour, and they are what the animation is built and tested for. The current i.MX (`mxcfb`) interface is still driven best-effort, which covers more devices than it once did: the Libra 2 and Clara 2E, and also the whole `kobo7` group (Clara HD, Forma, Nia, Libra H2O and the Edition 2 v2 boards), which issue the identical ioctl. The Reading-settings entry reflects this in three tiers: a supported (`hwtcon`) device shows a plain on/off row; a current i.MX (`mxcfb`) device keeps the on/off row but adds a caution that it may not work; anything else shows an *Unsupported* label in place of the toggle.

The **Release** column is what a published build does. **Developer** is the `NDS_PRERELEASE=1` build, which animates every interface the mod can decode so untested hardware can be evaluated; it is not published. *Tested by author* = personally run on the hardware.

| Device | Chipset | Interface | Release build | Developer build | Tested by author |
|---|---|---|---|---|---|
| **Clara BW** | MediaTek MT8113T | hwtcon (mono) | ✅ Supported | ✅ Animates | ✅ Yes |
| **Clara Colour** | MediaTek MT8113T | hwtcon (Kaleido) | ✅ Supported | ✅ Animates | ✅ Yes |
| **Libra Colour** | MediaTek MT8113T | hwtcon (Kaleido) | ✅ Supported | ✅ Animates | ✅ Yes |
| **Elipsa 2E** | MediaTek MT8113T | hwtcon (mono) | ✅ Supported (same family) | ✅ Animates | ❌ No |
| **Libra 2** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Clara 2E** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Clara HD** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Forma** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Libra H2O** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Aura Edition 2 v2** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Aura H2O Edition 2 v2** | NXP i.MX6SLL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Nia** | NXP i.MX6ULL | mxcfb (72) | ⚠️ Best effort | ✅ Animates | ❌ No |
| **Glo HD, Touch 2.0, Aura ONE** | NXP i.MX6SL | mxcfb (68) | ❌ Logs only | ✅ Animates (REAGL turns) | ❌ No |
| **Aura Edition 2 v1, Aura H2O Edition 2 v1** | NXP i.MX6SL | mxcfb (68) | ❌ Logs only | ✅ Animates (REAGL turns) | ❌ No |
| **Aura, Aura H2O** | NXP i.MX50 | mxcfb (64) | ❌ Logs only | ✅ Animates (REAGL turns) | ❌ No |
| **Touch, Glo, Mini, Aura HD** | NXP i.MX50 | mxcfb (68) | ❌ Logs only | ❌ Never (no REAGL waveform) | ❌ No |
| **Elipsa** | AllWinner B300 | sunxi (`DISP_EINK_UPDATE2`) | ⚠️ Best effort | ✅ Animates | ✅ Yes |
| **Sage** | AllWinner B300 | sunxi (`DISP_EINK_UPDATE2`) | ⚠️ Best effort | ✅ Animates | ❌ No |

Every row logs, including the ones that never animate. A device that produces no animation still produces a report, which is what makes the untested rows resolvable.

The "officially supported" test is the driver family, not the model: the mod treats the modern `hwtcon` interface as supported, the current `mxcfb` interface as unofficial best-effort, and the legacy `mxcfb` and `sunxi` interfaces as unsupported in a released build. A released build only ever sweeps `hwtcon` or the 72-byte `mxcfb`; on anything else it stays inert (no animation, no risk).

**The legacy i.MX interfaces are recognised but not driven.** The `mxcfb68` and `mxcfb64` rows in the platform table cover the i.MX6SL and i.MX50 devices. They exist so the mod can decode and log that hardware, which is what turns a silent unknown device into a usable report. In a released build it never animates there and the settings row reports the device as unsupported, and there is **no configuration key to override that**: an interface nobody has verified a page turn on is simply not supported, and no amount of config should be able to argue otherwise. The developer build (`NDS_PRERELEASE=1`) is the only thing that animates them, because establishing whether the wipe is viable is what that build is for.

Where those interfaces do animate, the sweep test is narrowed to REAGL and REAGLD only. That is deliberate: the i.MX50 kernels of the Touch, Glo, Mini and Aura HD carry no REAGL waveform at all, so those devices exclude themselves by what they emit and need no model list. The same 68-byte ioctl serves both the i.MX6SL (which has REAGL) and those i.MX50 boards (which do not), so the ioctl number alone cannot separate them, but the waveform can.

**Colour devices (Kaleido): how colour pages are skipped.** The strip-by-strip sweep can distort a *colour* page (the CFA colour conversion needs the whole frame), so colour pages must full-refresh instead of sweeping. The mod discriminates colour from B&W by the update's **waveform id**, not the CFA flag field. This was settled by reverse-engineering `libkobo` (Libra Colour, fw 4.45.23697):

- `KoboScreenMTK::handleAutoWfm(QRect, flags, hwtcon_update_data&)` resolves the waveform per render, calling `KoboScreenMTK::fbIsGray(QRect)`, Nickel's own per-region grayscale test. Grey content gets a **greyscale** waveform (`GLR16`=4 / `GL16`=3); non-grey content gets a **colour** waveform (`GCC16`=10 / `GLRC16`=11 / `GCK16`=8 / `GLKW16`=9). The id→name table is `KoboScreenMTK::idToWaveform`.
- The CFA colour-mode field (`HWTCON_FLAG_CFA_FLDS_MASK`/`0x7f00`) is a **device** property, not a content one: on-device logs show it set (mode G2, `flags=0x600`) on *every* reader update including B&W text, which is why the earlier field-based skip suppressed all animation.
- In the reader's **Dark Mode** the grey test still holds (a dark page is greyscale), but the waveforms shift to their dark counterparts: a reading turn is `GLKW16` (id 9, "REAGL DARK") and a full-flash is `GCK16` (id 8), the dark equivalents of `GLR16` and `GC16`. Confirmed on-device (Clara BW and Clara Colour).

So `nds_wf_sweepable` sweeps the greyscale reading turns: `GL16`/`GLR16` in day mode, plus `GLKW16` in Dark Mode. That test skips colour pages, the `GC16`/`GCK16` flashes, `AUTO`(257), and `A2`/`DU` menu updates, and because those are also the waveforms the home screen and menus use, it doubles as the reader-context guard (returning to the home screen never gets swept). On a mono panel (mxcfb, no CFA) the reading waveform varies by board revision: on-device logs show one Libra 2 turning with REAGL(6) and an older board with GLKW16(10), while both use AUTO(257)/DU for menus/toolbars and GC16 / full-screen AUTO for flashes. So the i.MX gate sweeps the flashless greyscale reading set (GL16=5, REAGL=6, REAGLD=7, GLKW16=10 in i.MX numbering) and rejects everything else, so menus and the toolbar never sweep. The animation is best-effort here: a REAGL board looks great, a GLKW16 board renders band-by-band and is slow, so the i.MX devices are not officially supported and the settings entry warns the mod may not work as expected on some revisions and can be turned off. Swept strips still set `HWTCON_FLAG_CFA_SKIP` to avoid per-region seams (`nds_debug_cfa_skip`); `nds_debug_sweep_any_waveform:1` bypasses the allowlist for experiments.

**Trying an untested device:** create a config with `nds_mode:observe` before the first reboot; it changes nothing and logs the ioctl stream. Confirm the log shows a platform (`[hwtcon]`/`[mxcfb]`) and `turn:` lines, then remove the line (or set `sweep`). An e-ink ioctl the mod does not drive is logged as an `UNKNOWN` line carrying the decoded request, so a device on an unhandled interface still produces a usable report rather than an empty log.

**Interface discovery (`ioctl-scan`).** Whenever tracing is on, the first sighting of each distinct ioctl request number is logged once, with no filtering on the `'F'` magic the rest of the tracing uses. That matters for a platform the mod does not model at all: a `sunxi` device would otherwise produce nothing, because its e-ink commands carry no `'F'` magic. A reading session costs a handful of `ioctl-scan:` lines and names every command the device actually issues. The argument is never dereferenced, since an unrecognised request carries no size to trust.

**The prerelease diagnostics build.** `./build.sh clean all strip koboroot NDS_PRERELEASE=1` produces a variant meant to be handed to owners of untested hardware. It animates every recognised e-ink interface (not only the ones with page-turn evidence) and traces from the first boot, so a tester only has to read a book normally and send back `nickel-dissolve.log` plus a note on whether the wipe looked right. Its startup block labels itself, so a report from it is never mistaken for a report about a release. It is not for general release: on hardware with no page-turn evidence the animation may look wrong.

### What the prerelease build is expected to do, per device

Predictions, not results. This is what the hardware map implies should happen, and the point of the build is to find out where it is wrong. Every row produces a log either way, including the rows expected to show no animation.

| Device | Platform | Interface | Expected outcome |
|---|---|---|---|
| Clara BW, Clara Colour | `kobo12` | `hwtcon` | Works. Tested by the author |
| Libra Colour | `kobo13` | `hwtcon` | Works. Tested by the author |
| Elipsa 2E | `kobo11` | `hwtcon` | Should work well. Same controller and same ioctl as the tested devices, untested only because nobody has run it |
| Libra 2 | `kobo9` | `mxcfb` (72) | Runs today, best effort. Quality varies by board revision |
| Clara 2E | `kobo10` | `mxcfb` (72) | Runs today, best effort. Same as the Libra 2 |
| Clara HD, Forma, Nia, Libra H2O, Aura Edition 2 v2, Aura H2O Edition 2 v2 | `kobo7` | `mxcfb` (72) | Should behave like the Libra 2. These use the identical ioctl and struct, so they are already picked up by the **current release** too, which the older table above does not say |
| Glo HD, Touch 2.0, Aura ONE, Aura Edition 2 v1, Aura H2O Edition 2 v1 | `kobo6` | `mxcfb68` | The most promising new ground. REAGL exists on this kernel, and the struct is one field away from the working one. Most likely to animate |
| Aura, Aura H2O | `kobo5` | `mxcfb64` | Uncertain. REAGL exists, but this is the only interface whose field offsets were never confirmed anywhere, and `libkobo` contains no 64-byte call at all |
| Touch, Glo, Mini, Aura HD | `kobo3`, `kobo4` | `mxcfb68` | **No animation expected.** These kernels have no REAGL waveform, so the sweep test rejects every turn. They still log, which is how the prediction gets checked |
| Sage, Elipsa | `kobo8` | `sunxi` | **Confirmed working on an Elipsa.** The Sage shares the interface and should behave the same, untested |

The two rows worth watching are `kobo6` (should work, and would roughly double the device count) and `kobo5` (rests on an unverified assumption). The `kobo3` / `kobo4` row is a deliberate negative prediction: if those devices *do* animate, the REAGL reasoning is wrong and the allowlist needs revisiting.

**The AllWinner `sunxi` devices needed a second update model, and now have one.** The Sage and Elipsa do not use the `mxcfb`-style interface. `libkobo` opens `/dev/disp` and `/dev/ion` and builds `disp_layer_config2` structures, so `KoboScreenAllWinner` drives the panel through the AllWinner disp2 layer API. Decoded on an Elipsa (fw 4.38.23697) and cross-checked against the `libkobo` disassembly:

```
Disp::flush(const QRect&, int, ulong)  ->  ioctl(fd, 0x406,  ulong ubuffer[8])   DISP_EINK_UPDATE2
Disp::waitFor(int)                     ->  ioctl(fd, 0x4014, buf)  buf[0] = frame id
```

`ubuffer[0]` points at a descriptor whose rect is an inclusive `left/top/right/bottom` at offsets 0 to 12; `ubuffer[2]` is the flush mode; `ubuffer[4]` points at the word the frame id comes back in. So the interface has the same send-plus-marker shape the rest of the mod relies on, and the sweep rewrites the rect in place and reissues, restoring it afterwards. Only the words it changes are touched, which avoids copying a descriptor whose total size is unknown.

Three things differ from every other platform, and each was learned the hard way:

- **The waveform cannot gate the sweep.** Every update carries the same mode whether it is a page turn, the status bar or a menu, so the gate is the armed turn plus a full-screen rect instead.
- **The band mode must be set explicitly.** Reusing the turn's own mode meant reusing `EINK_GC16_MODE` (`0x04`), the full black flash, so the first attempt flashed once per band and looked broken. The bands are forced to `0x420`, a flashless regional mode the device itself uses for status-bar redraws.
- **Every band must be waited on.** The update ioctl returns the frame id, and without waiting on each one the driver silently drops bands. The per-band wait also paces the sweep, so no artificial delay is needed.

Bands are wider here than anywhere else (0.69 in against 0.354 in), because a band costs a full completion-waited refresh on an older controller driving a large panel. That gives an Elipsa 9 bands and a Sage 7. The figure is set from what reads well on the hardware rather than derived.

**Why widening the platform table does not widen the risk.** Matching on the ioctl number is what keeps this safe, because the number encodes the size of its own argument. When the mod copies `plat->size` bytes out of the update Nickel passed, it is copying a struct whose length the kernel already agreed to, so a new row cannot read or write past the caller's buffer. The specific properties that hold on the legacy rows:

- **No waveform is ever invented.** `def_strip_wf` is 0 on every i.MX row, so each band reuses the waveform the device itself chose for that page. The mod never names a waveform id the hardware did not already ask for.
- **Nothing outside the update struct is touched.** No waveform data, no VCOM, no temperature, no PMIC register. The mod issues the same kind of partial update the device already uses for menus and toolbars.
- **`flags` is never written on i.MX.** That write is guarded by `cfa_skip`, which is zero on every mxcfb row, so the one field whose offset is unconfirmed on the 64-byte struct is only ever read, and only for a log line.
- **A rejected update is a skipped band, not a failure.** A strip the driver refuses returns an error and is stepped over; if the final marker never lands, the original full update is resubmitted so Nickel's wait always resolves.
- **The cold-wake sliver is off unless configured.** `nds_prewarm_ms` defaults to 0, so the 2-pixel lead-in, the one update whose geometry an older EPDC might reject on alignment grounds, never runs by default.

The worst outcome remains the one the mod was designed around: a frame that looks wrong until the next refresh fixes it. There is no path that writes hardware configuration.

## Build

```sh
./build.sh                                            # → KoboRoot.tgz  (podman + ghcr.io/pgaskin/nickeltc)
./build.sh clean all strip koboroot NDS_PRERELEASE=1  # → the diagnostics build (see Device support)
```

Copy `KoboRoot.tgz` to the device's `.kobo/`, eject, and reboot. The animation is on after that
reboot, no configuration needed.
