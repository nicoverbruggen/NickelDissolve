# Changelog

All notable, user-visible changes to NickelDissolve. Release notes are generated from this
file: each `## vX.Y` heading must exactly match a git tag.

## Unreleased

### Added

- Continuous integration: build, firmware-symbol check, and CHANGELOG-driven release workflows.
- Firmware symbol verification (`test/syms`) against real firmware dumps, with `//libnickel`
  annotations on the hooked `ReadingView` page-turn symbols.
- The running mod version and firmware version are now logged in the startup block (and the mod
  version prefixes every log line).

### Changed

- Standardized logging: the on-device log is now `nickel-dissolve.log`, is capped in size
  (rotates to `.log.old`), and timestamps are thread-safe.
- Page-turn / ioctl tracing is now tied to the mode: a supported device running the animation
  (`nds_mode:sweep`) stays quiet. Use `nds_mode:observe` or `nds_log:1` to trace. The old
  `nds_debug_log_ioctl` key (which logged by default) is replaced by `nds_log`.
- A malformed config now turns on verbose logging for that boot so it diagnoses itself.

## v0.1

### Added

- Initial release: Kindle-style directional page-turn band-wipe for Kobo, driven entirely
  through the public e-ink update ioctl. Works on hwtcon (MediaTek), mxcfb (i.MX), and sunxi
  (AllWinner) devices; colour (Kaleido) pages are detected and refresh normally.
- Per-gesture control (`nds_animate_on_swipe` / `_tap` / `_button`) and per-device tuned
  defaults. Verified on the Kobo Libra Colour, including colour-page handling.
