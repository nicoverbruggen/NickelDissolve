# Changelog

All notable, user-visible changes to NickelDissolve. Release notes are generated from this
file: each `## vX.Y` heading must exactly match a git tag.

## Unreleased

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
