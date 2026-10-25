# Contributing

Thanks for contributing.

## Development Principles

- Keep project-owned code original-first.
- Follow [CLEAN_ROOM_POLICY.md](./CLEAN_ROOM_POLICY.md).
- Preserve upstream licensing in vendored trees.
- Contributions to project-owned code are accepted under the MIT License (see `LICENSE`).

## Pull Request Requirements

Include this section in every PR description:

### Provenance

- Sources consulted:
- Copied code: none / yes (list files and sources)
- License impact: none / describe

## Practical Rules

- Prefer implementing behavior from specs/docs and runtime tests.
- Do not copy upstream implementation code into `host/` unless explicitly intended and attributed.
- If intentional reuse occurs, preserve headers and update `THIRD_PARTY_NOTICES.md`.

## Build and Test

Common local flow:

```powershell
cmake --preset msvc-release
cmake --build --preset build-msvc-release
.\build\Release\emugbds.exe --help
```
