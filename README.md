# NickelDissolve

> [!WARNING]
> **Work in progress — not ready for general use!** This is an experimental prototype under active development. Only a few devices are verified, colour-page handling is incomplete, and the configuration and behaviour may still change. Try it only if you know what you're doing and don't mind rough edges. (It's reversible and shouldn't brick your device — see [Is it safe?](#is-it-safe) — but there are no guarantees.)

NickelDissolve gives your Kobo a **Kindle-style page-turn animation**: instead of the whole page changing at once, the new page sweeps in from the side as a smooth band-wipe.

## What it does

If you've seen a recent Kindle turn a page, you know the effect: the new page glides across the screen instead of appearing in one flat blink. Kobo devices use the same E Ink screen technology, so NickelDissolve recreates that animation on your Kobo.

Out of the box:

- **Swiping** to turn a page plays the animation, forward and backward (backward turns sweep the other way).
- **Tapping** to turn a page stays instant — a deliberate split many people like. (You can change this.)
- Everything else — menus, the home screen, images — is completely untouched.

It's a stepped sweep rather than a perfectly fluid one (that's a limit of how E Ink screens accept updates), but with the default settings it looks pretty good!

## Which devices work?

| Device | Works? |
|---|---|
| Kobo Libra Colour | ✅ Yes, tested |
| Kobo Clara Colour | ✅ Yes, tested |
| Kobo Clara BW | ✅ Yes, tested |
| Kobo Libra 2 | ✅ Should work (not yet verified) |
| Kobo Clara 2E | ✅ Should work (not yet verified) |
| Kobo Elipsa 2E | ✅ Should work (not yet verified) |
| Kobo Elipsa / Sage | ⚠️ Works, but too slow to recommend — off unless you opt in |
| Anything else | Nothing happens: the mod stays inactive, no animation and no risk |

On colour devices (Kaleido screens), pages with **colour content** don't animate — they refresh normally, because animating them would distort the colours. Black-and-white pages (regular books) animate as usual.

## Install

1. Download `KoboRoot.tgz` and copy it into the hidden `.kobo` folder on your Kobo.
2. Safely eject; the device reboots and installs the mod.
3. That's it — swipe to turn a page and it wipes in. No setup needed.

## Removing or turning it off

- **Remove completely:** delete the file `KOBOeReader/.adds/nickel-dissolve/uninstall` and reboot. The mod removes itself.
- **Turn off but keep installed:** create a text file named `config` in `KOBOeReader/.adds/nickel-dissolve/` containing the line `nds_mode:off`, then reboot.

## Customizing

You don't need a configuration file — the defaults are tuned per device. If you want to tweak, create a plain-text file named `config` in `KOBOeReader/.adds/nickel-dissolve/` with one setting per line. A few popular ones:

```
nds_delay_us:30000    # slow the animation down
nds_direction:ltr     # sweep left-to-right instead
nds_tap_animates:1    # animate taps too, not just swipes
```

Changes apply on the next reboot; delete the file to go back to the defaults. The `doc` file in the same folder on your device explains every setting, and the full reference lives in [ABOUT.md](ABOUT.md).

## Is it safe?

- Only the page-turn refresh is ever touched; everything else on screen works exactly as before.
- The mod can't get stuck holding your screen: if anything about the animation fails, the normal page refresh is shown instead.
- If a page ever looks garbled, the next refresh fixes it — nothing is permanent.
- On unknown devices or after a firmware change it doesn't recognise, the mod simply goes inactive rather than guessing.
- It removes itself cleanly (see above), leaving your Kobo exactly as it was.

## Technical details

Curious how it works, what every configuration value (including the `nds_debug_*` settings) does, why each device gets different defaults, or what's on the roadmap? See **[ABOUT.md](ABOUT.md)** — the technical companion to this README, including build instructions.

## Credits

Built with support from large language models, and with prior Kobo modding experience from:

- [NickelHome](https://github.com/nicoverbruggen/NickelHome)
- [NickelTypeFix](https://github.com/nicoverbruggen/NickelTypeFix)
- [NickelCoverFix](https://github.com/nicoverbruggen/NickelCoverFix)
