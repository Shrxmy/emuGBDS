# emuGBDS Docs

If you are new to this codebase, read in this order:

1. `docs/CODEBASE_TOUR.md`
2. `docs/WORKFLOW.md`
3. `docs/ROADMAP.md`

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

Run GBA (interactive):

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --interactive
```

Run GBA with debug stats:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 600 --debug-stats
```

Run DS firmware Slot-2 handoff test:

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --boot-mode ds-firmware --trace-transition --frames 300
```

## Important Notes

- `bios/` and `roms/` are local assets. Do not commit them.
- This repository follows a clean-room approach (`CLEAN_ROOM_POLICY.md`).
- The in-house GBA core (`ingba`) and DS core (`inds`) are the active implementation targets.
