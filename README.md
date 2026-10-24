# emuGBDS

Clean-room emulator host focused on in-house core development.

Current state:
- In-house GBA core scaffold (`ingba`) with incremental ARM execution support.
- In-house DS core scaffold (`inds`) with dual-CPU timing skeleton and firmware Slot-2 transition support.
- Optional external melonDS adapter support behind `EMUGBDS_ENABLE_MELONDS`.

## Documentation

Start with: `docs/README.md`

Architecture overview: `docs/ARCHITECTURE.md`

## Build

```powershell
cmake --preset msvc-release
cmake --build --preset build-msvc-release
```

Optional (if you provide a local `melonDS-master/` source tree):

```powershell
cmake --preset nds-clang-release
cmake --build --preset build-nds-clang-release
```

## Run

Headless in-house GBA smoke run:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 300
```

Interactive in-house GBA run (SDL window):

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --interactive
```

Optional uncapped interactive run:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --interactive --uncapped
```

In-house DS ROM run:

```powershell
.\build\Release\emugbds.exe --mode ds --nds .\roms\Fire_Emblem_Shadow_Dragon.nds --frames 300
```

DS firmware Slot-2 handoff (no `.nds` inserted, transition to GBA):

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --boot-mode ds-firmware --trace-transition --frames 300
```

Interactive controls:

- `Z` = `A`
- `X` = `B`
- `Enter` = `Start`
- `Right Shift` = `Select`
- Arrow keys = D-pad
- `A` = `L`
- `S` = `R`
- `Esc` = exit interactive loop

## Test

Configure and build:

```powershell
cmake --preset msvc-release
cmake --build --preset build-msvc-release
```

Run all tests:

```powershell
ctest --test-dir .\build -C Release --output-on-failure
```

## Privacy

`roms/` and `bios/` are intended for local development assets and should not be committed.
