# Third-Party Notices

This repository contains third-party source trees. Their original licenses and notices remain in force.

## Repository License Scope

- The top-level `LICENSE` (MIT) applies to project-owned code.
- Third-party source trees listed below remain under their own licenses.
- Distributing combined builds must satisfy all applicable licenses.

## Included Upstream Projects

1. SDL
- Path: `sdl/`
- Upstream: https://github.com/libsdl-org/SDL
- License: zlib (see distributed SDL license files)

2. Dear ImGui (v1.91.9b, core files and the SDL2 / SDL_Renderer2 backends)
- Path: `host/third_party/imgui/`
- Upstream: https://github.com/ocornut/imgui
- License: MIT (see `host/third_party/imgui/LICENSE.txt`)

## Project-Owned Code

Project-owned code currently lives primarily under `host/` and top-level project configuration files.

Project-owned code is licensed under the MIT License (see `LICENSE`).
The project also follows the original-first clean-room policy documented in `CLEAN_ROOM_POLICY.md`.

## When Adding New Third-Party Code

Contributors must:

1. Keep the original license text and headers.
2. Add an entry in this file (name, path, source, license).
3. Mention provenance in the PR description.

This file is informational and not legal advice.
