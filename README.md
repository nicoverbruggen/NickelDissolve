# NickelDissolve

> [!WARNING]
> **Initial, experimental release.** Officially supported on the Kobo **Libra Colour**, **Clara Colour**, and **Clara BW**. On i.MX-based devices (Libra 2, Clara 2E) it **⚠️ may work depending on the board revision**, because only later revisions support the fast, high-quality page-turn refresh it relies on. Behaviour and configuration may still change. It's reversible and shouldn't brick your device (see [Is it safe?](#is-it-safe)), but there are no guarantees.

NickelDissolve gives your Kobo a **Kindle-style page-turn animation**: instead of the whole page changing at once, the new page sweeps in from the side as a smooth band-wipe.

See it in action: **[short demo video](https://www.youtube.com/shorts/viUPrkyF2Yg)**.

## What it does

If you've seen a recent Kindle turn a page, you know the effect: the new page glides across the screen instead of appearing in one flat blink. Kobo devices use the same E Ink screen technology, so NickelDissolve recreates that animation on your Kobo.

Out of the box:

- **Every way of turning a page animates:** swipes, taps, and the physical page-turn buttons. Backward turns sweep the other way, and a tap even sweeps toward the side you tapped.
- Each gesture can be turned off individually if you prefer, say, instant taps with animated swipes.
- Everything else (menus, the home screen, images) is completely untouched.

It's a stepped sweep rather than a perfectly fluid one (that's a limit of how E Ink screens accept updates), but with the default settings it looks pretty good!

## Which devices work?

| Device | Works? |
|---|---|
| Kobo Libra Colour | ✅ Officially supported, tested (including colour-page handling) |
| Kobo Clara Colour | ✅ Officially supported (same hardware family as the Libra Colour) |
| Kobo Clara BW | ✅ Officially supported, tested (black-and-white) |
| Kobo Libra 2 | ⚠️ May work (depends on device revision) |
| Kobo Clara 2E | ⚠️ May work (depends on device revision) |
| Kobo Elipsa 2E | ⚠️ May work (not yet verified) |
| Kobo Elipsa / Sage | ⚠️ Works, but too slow to recommend; off unless you opt in |
| Anything else | Nothing happens: the mod stays inactive, no animation and no risk |

**Officially supported: the Kobo Libra Colour, Clara Colour, and Clara BW.** On these the animation is reliable.

**A note on i.MX devices (Libra 2, Clara 2E):** these shipped in several board revisions, and only the later ones support the fast, high-quality page-turn refresh the animation relies on. On an older revision it can look slow or fall back to full-screen flashes. Working out which revisions can handle it is still in progress. If you have one, a page-turn log on **[issue #1](https://github.com/nicoverbruggen/NickelDissolve/issues/1)** helps a lot.

On colour devices (Kaleido screens), pages with **colour content** are detected automatically and refresh normally instead of animating, so colours are never distorted. Regular black-and-white pages animate as usual.

## Install

1. Download `KoboRoot.tgz` and copy it into the hidden `.kobo` folder on your Kobo.
2. Safely eject; the device reboots and installs the mod.
3. That's it. Swipe to turn a page and it wipes in. No setup needed.

## Removing or turning it off

- **Turn off or on from Settings (easiest):** on your Kobo, open **Settings**, go to **Reading**, and under **Page Appearance** you'll find a **Page turn animations:** switch, right alongside the built-in options like Dark Mode. Uncheck it to turn the animation off, check it to turn it back on. The change takes effect on the very next page turn and is remembered after a reboot, so there's no file to edit. (On the rare device where this switch can't be added, use the config-file method below instead.)
- **Remove completely:** delete the file `KOBOeReader/.adds/nickel-dissolve/uninstall` and reboot. The mod removes itself.
- **Turn off but keep installed (config file):** create a text file named `config` in `KOBOeReader/.adds/nickel-dissolve/` containing the line `nds_mode:off`, then reboot.

## Customizing

You don't need a configuration file; the defaults are tuned per device. If you want to tweak, create a plain-text file named `config` in `KOBOeReader/.adds/nickel-dissolve/` with one setting per line. A few popular ones:

```
nds_delay_us:30000      # slow the animation down
nds_direction:ltr       # sweep left-to-right instead
nds_animate_on_tap:0    # taps turn instantly; swipes and buttons still animate
```

Changes apply on the next reboot; delete the file to go back to the defaults. The `doc` file in the same folder on your device explains every setting, and the full reference lives in [ABOUT.md](ABOUT.md).

## Reporting a problem

If something doesn't look right, the mod keeps a small log file that makes it much easier to help you. Connect your Kobo to a computer over USB and look for:

```
KOBOeReader/.adds/nickel-dissolve/nickel-dissolve.log
```

Attach that file when you report an issue. It records which version of the mod you're running and your Kobo's firmware, and it normally stays short. If you're asked for more detail, open the `config` file in the same folder, add the line `nds_log:1`, reboot, and try again. That captures a full page-turn trace in the log.

## Is it safe?

- Only the page-turn refresh is ever touched; everything else on screen works exactly as before.
- The mod can't get stuck holding your screen: if anything about the animation fails, the normal page refresh is shown instead.
- If a page ever looks garbled, the next refresh fixes it. Nothing is permanent.
- On unknown devices or after a firmware change it doesn't recognise, the mod simply goes inactive rather than guessing.
- It removes itself cleanly (see above), leaving your Kobo exactly as it was.

## Technical details

Curious how it works, what every configuration value (including the `nds_debug_*` settings) does, or why each device gets different defaults? See **[ABOUT.md](ABOUT.md)**, the technical companion to this README. To build it yourself or contribute, see **[CONTRIBUTING.md](CONTRIBUTING.md)**.

## Credits

Built on [NickelHook](https://github.com/pgaskin/NickelHook) by Patrick Gaskin.

This mod was created by the author with the help of the following large language models:

- Claude Opus 4.8
- Claude Fable
