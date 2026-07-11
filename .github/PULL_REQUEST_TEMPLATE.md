## What & why

<!-- What does this change, and what problem does it solve? Link the issue if there is one. -->

## Tested on

<!-- Device + firmware you actually ran this on. "Builds but untested on hardware" is fine to
     say — just say it. -->

- Device:
- Firmware:

## Log

<!-- Paste the relevant excerpt from KOBOeReader/.adds/nickel-dissolve/nickel-dissolve.log for
     the affected flow (the startup block plus the lines around your change). Set nds_log:1 or
     nds_mode:observe to capture the page-turn traces. -->

```text

```

## Checklist

- [ ] `CHANGELOG.md` has an entry under `## Unreleased` (required for user-visible changes).
- [ ] New/changed libnickel symbols carry a `//libnickel <first> <last|*> <symbol>` annotation.
- [ ] All hooks/dlsyms remain `.optional = true` and are null-checked at the use site.
- [ ] `./build.sh` succeeds.
