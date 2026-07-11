# NickelDissolve

> [!WARNING]
> **Early release (v0.1) — experimental.** Verified on the Kobo Libra Colour so far (including colour-page handling); other devices are expected to work but haven't been confirmed on hardware. Behaviour and configuration may still change. It's reversible and shouldn't brick your device — see [Is it safe?](#is-it-safe) — but there are no guarantees.

> [!NOTE]
> **Testers wanted!** I'd love confirmation on other Kobo models — especially the Clara Colour, Clara BW, Libra 2, Clara 2E, and Elipsa 2E. If you can try it and report back (a page-turn log helps a lot), please comment on **[issue #1](https://github.com/nicoverbruggen/NickelDissolve/issues/1)**.

NickelDissolve gives your Kobo a **Kindle-style page-turn animation**: instead of the whole page changing at once, the new page sweeps in from the side as a smooth band-wipe.

## What it does

If you've seen a recent Kindle turn a page, you know the effect: the new page glides across the screen instead of appearing in one flat blink. Kobo devices use the same E Ink screen technology, so NickelDissolve recreates that animation on your Kobo.

Out of the box:

- **Every way of turning a page animates** — swipes, taps, and the physical page-turn buttons. Backward turns sweep the other way, and a tap even sweeps toward the side you tapped.
- Each gesture can be turned off individually if you prefer, say, instant taps with animated swipes.
- Everything else — menus, the home screen, images — is completely untouched.

It's a stepped sweep rather than a perfectly fluid one (that's a limit of how E Ink screens accept updates), but with the default settings it looks pretty good!

## Which devices work?

| Device | Works? |
|---|---|
| Kobo Libra Colour | ✅ Yes, tested (including colour-page handling) |
| Kobo Clara Colour | ✅ Yes (same hardware family as the Libra Colour) |
| Kobo Clara BW | ✅ Yes (black-and-white; nothing colour to handle) |
| Kobo Libra 2 | ✅ Should work (not yet verified) |
| Kobo Clara 2E | ✅ Should work (not yet verified) |
| Kobo Elipsa 2E | ✅ Should work (not yet verified) |
| Kobo Elipsa / Sage | ⚠️ Works, but too slow to recommend — off unless you opt in |
| Anything else | Nothing happens: the mod stays inactive, no animation and no risk |

On colour devices (Kaleido screens), pages with **colour content** are detected automatically and refresh normally instead of animating, so colours are never distorted. Regular black-and-white pages animate as usual.

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
nds_delay_us:30000      # slow the animation down
nds_direction:ltr       # sweep left-to-right instead
nds_animate_on_tap:0    # taps turn instantly; swipes and buttons still animate
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
