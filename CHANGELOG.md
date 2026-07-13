# Changelog

All notable, user-visible changes to NickelDissolve. Release notes are generated from this
file: each `## vX.Y` heading must exactly match a git tag.

## Unreleased

### Changed

- **Official device support is now the modern Kobo devices only: Clara BW, Clara Colour, and Libra
  Colour.** The older i.MX devices (Libra 2, Clara 2E) and the large-panel Elipsa/Sage are no longer
  officially supported. Their code is still in the mod, so you can install and run it on them if you
  like, but it may not work well.

### Added

- **The Reading-settings entry now tells you whether your device is supported.** On a supported
  device it keeps the on/off toggle and adds a line reading "Your device's hardware revision supports
  page turn animations." On an unsupported device the toggle is replaced with an "Unsupported" label
  and a line explaining that page turn animations are not possible on that hardware.
- **The startup log now records the device's hardware revision** (from the NTX hardware-config block),
  which helps diagnose support questions from a log alone.

## v0.3

### Added

- A **"Page turn animations:" setting in the Reading settings page**, in the Page Appearance
  section alongside the built-in options, so you can turn the animation on or off without editing
  a config file. It is built to match the native rows exactly, takes effect on the next page turn,
  and is remembered across reboots. On any device where the setting cannot be added cleanly it is
  simply left out, and the config file still works as before.

### Fixed

- **The first-page-turn animation can no longer fire outside a book.** The special handling that
  turns the forced full refresh on a book's first page turn into a smooth animation was only
  guarded by the page-turn buttons, so in rare cases the home screen or a menu could be animated
  by mistake. The mod now tracks whether the reader is actually on screen and only applies that
  first-turn handling when a book is genuinely open.

### Improved

- **Smoother page-turn animation on i.MX devices such as the Kobo Libra 2.** These panels pace the
  wipe with a fixed delay between bands instead of the hardware timing the newer panels use, and
  the default delay is now a little longer so each band settles cleanly before the next. This
  removes the slight steppiness on those devices. Tuned and confirmed on the Kobo Libra 2, which is
  now hardware-verified.

## v0.2

Reliability and polish for the page-turn animation, confirmed on the Kobo Clara BW and Kobo
Libra Colour.

### Fixed

- **Tap page turns now animate.** Taps were misclassified as swipes because the gesture filter
  read widget-local touch coordinates; it now uses screen coordinates, so a tap is recognised as
  a tap and its turn animates (and sweeps toward the side you tapped).

### Changed

- **Opening a book is now a clean full refresh, and the first page turn animates.** Kobo forces
  a full flash on the first turn after opening; the mod now renders that turn as a normal
  animated sweep instead. Set `nds_animate_first_turn:0` to keep Kobo's flash on the first turn.
- The render that immediately follows a book open or chapter jump is no longer mistaken for a
  page turn, so opening a book no longer briefly animates its first page.

## v0.1

A Kindle-style directional page-turn animation for Kobo: the new page sweeps in as a band-wipe
across the screen instead of a single flat refresh. It works entirely through the e-ink screen's
public update interface, with no driver patching.

### Added

- Page-turn animation for every way of turning a page: swipes, taps, and the physical
  page-turn buttons. Backward turns sweep the other way, and a tap sweeps toward the side you
  tapped.
- Per-gesture control (`nds_animate_on_swipe`, `nds_animate_on_tap`, `nds_animate_on_button`)
  and tuning options for the number of bands (`nds_strips`), speed (`nds_delay_us`), and
  direction (`nds_direction`), with sensible defaults tuned per device. `nds_mode` chooses
  `sweep` (default), `observe`, or `off`.
- Automatic colour-page handling on Kaleido colour panels: pages with colour content refresh
  normally instead of animating, so colours are never distorted.

### Device support

- **Verified on hardware:** Kobo Libra Colour (including colour-page handling) and Kobo Clara BW.
- **Expected to work (not yet hardware-verified):** Kobo Clara Colour, Libra 2, Clara 2E,
  Elipsa 2E.
- **Works but off by default** (the wipe is slow on the large panels): Kobo Elipsa and Sage.
  Opt in with `nds_mode:sweep`.
- **Any other device:** the mod stays inactive, with no animation and no risk.

### Safety

- Only a black-and-white reading page-turn is ever animated; menus, the home screen, and colour
  content pass through untouched.
- If a sweep ever fails, the normal page refresh is shown instead. A bad frame is fixed by the
  next refresh, never a brick. On an unrecognised device or firmware the mod simply stays
  inactive.
- Reversible: delete the `uninstall` file and reboot to remove it cleanly.
