# emuGBDS

Prototype host project with:

- an in-house clean-room GBA core (`ingba`)
- optional third-party integrations:
- `melonDS` for Nintendo DS
- `mGBA` for Game Boy Advance

## Original-First Policy

To keep new host code original while still learning from established emulators:

- Clean-room process: [CLEAN_ROOM_POLICY.md](./CLEAN_ROOM_POLICY.md)
- Contributor requirements: [CONTRIBUTING.md](./CONTRIBUTING.md)
- Third-party attributions: [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)

## License

- Project-owned code is licensed under `GPL-3.0-or-later` (see [LICENSE](./LICENSE)).
- Vendored third-party trees keep their original upstream licenses.
- See [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) for per-component details.

This repository currently implements a first executable (`emugbds`) with:

- One host API (`IEmuCore`, `ModeCoordinator`, `BootRequest`, `TransitionEvent`)
- Runtime kernel architecture (`RuntimeKernel`, `BootConfig`, `TickResult`, `OutputBundle`)
- Unified host input contract (`InputSnapshot`) with runtime-owned key injection
- Boot policy subsystem (`BootPolicy`) for default BIOS/firmware path resolution
- Reusable frame pacing subsystem (`FrameScheduler`)
- Runtime-owned AV pipelines (`VideoPipeline`, `AudioPipeline`) consumed by SDL frontend
- DS adapter (`MelonDsCoreAdapter`)
- GBA adapter (`MGbaCoreAdapter`)
- In-house clean-room GBA adapter scaffold (`InHouseGbaCoreAdapter`, `ingba::Core`)
- melonDS platform callback shim required for embedding `core` outside the original frontend
- DS -> GBA transition hook when melonDS signals `GBAModeNotSupported`

## Build

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Or use presets:

```powershell
cmake --preset msvc-release
cmake --build --preset build-msvc-release
```

For optional third-party NDS + GBA integration on Windows, use:

```powershell
cmake --preset thirdparty-clang-release
cmake --build --preset build-thirdparty-clang-release
```

Requires Visual Studio ClangCL tools (or LLVM/clang in your PATH).

### Defaults

By default, third-party cores are disabled (`EMUGBDS_ENABLE_MELONDS=OFF`, `EMUGBDS_ENABLE_MGBA=OFF`) so the project builds from in-house code only.

## Run

```powershell
.\build\Release\emugbds.exe --mode ds --nds path\to\game.nds --slot2 path\to\slot2.gba --frames 300
```

or:

```powershell
.\build\Release\emugbds.exe --mode gba --gba path\to\game.gba --frames 300
```

In-house clean-room GBA core smoke path:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 300
```

To select backend explicitly:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 300
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core mgba --sdl
```

### DS firmware boot mode and BIOS-authentic GBA switch

If these files exist, they are auto-detected:

- `bios\nds-bios\biosnds7.bin`
- `bios\nds-bios\biosnds9.bin`
- `bios\nds-bios\dsfirmware.bin`

When all 3 are present, `--boot-mode auto` resolves to `ds-firmware`; otherwise it resolves to `direct`.

Example:

```powershell
.\build\Release\emugbds.exe --mode ds --nds .\roms\Fire_Emblem_Shadow_Dragon.nds --slot2 .\roms\Fire_Emblem.gba --boot-mode ds-firmware --trace-transition --frames 20000
```

Or explicit paths:

```powershell
.\build\Release\emugbds.exe --mode ds --nds .\roms\Fire_Emblem_Shadow_Dragon.nds --slot2 .\roms\Fire_Emblem.gba --bios-nds7 .\bios\nds-bios\biosnds7.bin --bios-nds9 .\bios\nds-bios\biosnds9.bin --firmware-nds .\bios\nds-bios\dsfirmware.bin --boot-mode ds-firmware --trace-transition
```

### SDL test frontend (GBA)

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --sdl --scale 3
```

Note: SDL frontend currently requires `--gba-core mgba`. The in-house core is headless in this milestone.

For deterministic SDL smoke tests (auto-exit after N frames):

```powershell
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba --sdl --scale 3 --sdl-frames 900
```

Speed control:

```powershell
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba --sdl --speed 1.0
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba --sdl --speed 0.85   # slower
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba --sdl --speed 1.20   # faster
```

You can also pass ROM path positionally:

```powershell
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba --sdl --scale 3
```

If `bios\gb-bios\gba_bios.bin` exists, it is auto-detected.  
To force a specific BIOS file, pass:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --bios-gba .\bios\gb-bios\gba_bios.bin --sdl
```

Controls:

- `X` = A
- `Z` = B
- `A` = L
- `S` = R
- `Enter` = Start
- `Backspace` = Select
- Arrow keys = D-pad
- `Esc` = Quit

## Notes

- This is an integration baseline, not full feature parity yet.
- Save/state plumbing and full frontend UX are intentionally incremental.
- Upstream licensing obligations still apply (`GPLv3` from melonDS and `MPL-2.0` from mGBA).
