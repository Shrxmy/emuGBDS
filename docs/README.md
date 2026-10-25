# emuGBDS Docs

If you are new to this codebase, read in this order:

1. `docs/ARCHITECTURE.md`
2. `docs/CODEBASE_TOUR.md`
3. `docs/WORKFLOW.md`
4. `docs/ROADMAP.md`

## Quick Start

Build:

```powershell
cmake --preset msvc-release
cmake --build --preset build-msvc-release
```

Run tests:

```powershell
ctest --test-dir .\build -C Release --output-on-failure
```

Open the app (then File > Open ROM, or drop a ROM on the window), or start a game directly:

```powershell
.\build\Release\emugbds.exe
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba
```

Run GBA with debug stats:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 600 --debug-stats
```

Run the DS firmware menu with a Slot-2 GBA game (tap "Start GBA game" or press A):

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --interactive
```

## Important Notes

- `bios/` and `roms/` are local assets. Do not commit them.
- This repository follows a clean-room approach (`CLEAN_ROOM_POLICY.md`).
- The in-house GBA core (`ingba`) and DS core (`inds`) are the active implementation targets.
