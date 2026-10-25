# Clean-Room Development Policy

This repository aims to build an original emulator host while interoperating with upstream emulator projects.

This document defines how to avoid accidental code-copy contamination.

## Scope

- Applies to all code under `host/` and any newly created project-owned directories.
- Does not change the original licenses of vendored third-party trees (`sdl/`, `host/third_party/`).
- Project-owned code licensing is documented in the top-level `LICENSE` (MIT).

## Core Rule

Write project-owned code from scratch based on:

- hardware documentation,
- public APIs,
- runtime behavior,
- test results.

Do not copy/paste implementation code from upstream emulator repositories into project-owned files unless the contribution is intentionally treated as third-party derived code with explicit attribution and compatible licensing.

## Allowed Inputs

- Official hardware documentation and specs (for example GBATEK).
- Upstream public interfaces, command usage, and observable behavior.
- Your own notes, diagrams, and pseudocode written in your own words.

## Disallowed Inputs for Project-Owned Files

- Verbatim or lightly edited code copied from upstream emulator source.
- Structure-preserving rewrites (same control flow and variable sequence) when the source is an upstream implementation.
- AI prompts that request “rewrite this upstream file” into host-owned files.

## Two-Person Clean-Room Workflow (Recommended)

Use this when implementing behavior known from upstream codebases:

1. Research role:
- reads upstream code and docs,
- writes a behavior/spec note only (no code snippets, no near-verbatim pseudocode).
2. Implementation role:
- does not view upstream implementation files,
- implements from the behavior/spec note and hardware docs only.

For solo development, emulate this separation by writing a short behavior note first, then implementing in a fresh pass without upstream files open.

## Mandatory Provenance Note for New Features

Each feature PR should include a short provenance section:

- "Sources consulted" (docs/specs/behavior),
- "Copied code: none" or explicit attribution if copied,
- "License impact" statement if copied material exists.

## If Copying Is Intentional

If you intentionally reuse upstream code:

1. Keep it isolated in a clearly named directory/file.
2. Preserve original copyright/license headers.
3. Record it in `THIRD_PARTY_NOTICES.md`.
4. Confirm resulting distribution obligations before release.

## Enforcement Checklist

Before merge:

- [ ] New host-owned code is original and not copied.
- [ ] Feature PR includes provenance note.
- [ ] Any intentional third-party code is attributed and tracked.

This policy is engineering guidance, not legal advice.
