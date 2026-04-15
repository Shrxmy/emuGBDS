# emuGBDS

Prototype host project that unifies:

- `melonDS` as the Nintendo DS runtime core
- `mGBA` as the Game Boy Advance runtime core

This repository currently implements a first executable (`emugbds`) with:

- One host API (`IEmuCore`, `ModeCoordinator`, `BootRequest`, `TransitionEvent`)
- DS adapter (`MelonDsCoreAdapter`)
- GBA adapter (`MGbaCoreAdapter`)
- melonDS platform callback shim required for embedding `core` outside the original frontend
- DS -> GBA transition hook when melonDS signals `GBAModeNotSupported`

## Build

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Windows/MSVC default

On MSVC, `EMUGBDS_ENABLE_MELONDS` currently defaults to `OFF` in this repository because upstream melonDS core sources hit compiler-compat issues in this toolchain.  
You can still build and run the unified host with mGBA integration immediately.

To try enabling melonDS integration explicitly:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEMUGBDS_ENABLE_MELONDS=ON
```

## Run

```powershell
.\build\Release\emugbds.exe --mode ds --nds path\to\game.nds --slot2 path\to\slot2.gba --frames 300
```

or:

```powershell
.\build\Release\emugbds.exe --mode gba --gba path\to\game.gba --frames 300
```

### SDL test frontend (GBA)

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --sdl --scale 3
```

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
- Licensing obligations from upstream projects still apply (`GPLv3` from melonDS and `MPL-2.0` from mGBA).
