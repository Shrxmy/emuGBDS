# Third-Party Notices

This repository contains third-party source trees. Their original licenses and notices remain in force.

## Repository License Scope

- The top-level `LICENSE` (`GPL-3.0-or-later`) applies to project-owned code.
- Third-party source trees listed below remain under their own licenses.
- Distributing combined builds must satisfy all applicable licenses.

## Included Upstream Projects

1. melonDS
- Path: `melonDS-master/`
- Upstream: https://github.com/melonDS-emu/melonDS
- License: GPL-3.0-or-later (see upstream `LICENSE`)

2. mGBA
- Path: `mgba-master/`
- Upstream: https://github.com/mgba-emu/mgba
- License: MPL-2.0 (see upstream `LICENSE`)

3. SDL
- Path: `sdl/`
- Upstream: https://github.com/libsdl-org/SDL
- License: zlib (see distributed SDL license files)

## Project-Owned Code

Project-owned code currently lives primarily under `host/` and top-level project configuration files.

Project-owned code is licensed under `GPL-3.0-or-later`.
The project also follows the original-first clean-room policy documented in `CLEAN_ROOM_POLICY.md`.

## When Adding New Third-Party Code

Contributors must:

1. Keep the original license text and headers.
2. Add an entry in this file (name, path, source, license).
3. Mention provenance in the PR description.

This file is informational and not legal advice.
