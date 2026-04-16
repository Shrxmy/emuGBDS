# emuGBDS

Clean-room emulator host focused on in-house core development.

Current state:
- In-house GBA core scaffold (`ingba`) with incremental ARM execution support.
- In-house DS core scaffold (`inds`) with dual-CPU timing skeleton and firmware Slot-2 transition support.
- Optional external melonDS adapter support behind `EMUGBDS_ENABLE_MELONDS`.

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

In-house DS ROM run:

```powershell
.\build\Release\emugbds.exe --mode ds --nds .\roms\Fire_Emblem_Shadow_Dragon.nds --frames 300
```

DS firmware Slot-2 handoff (no `.nds` inserted, transition to GBA):

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --boot-mode ds-firmware --trace-transition --frames 300
```

## Privacy

`roms/` and `bios/` are intended for local development assets and should not be committed.
