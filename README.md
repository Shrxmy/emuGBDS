# emuGBDS

Clean-room emulator host focused on in-house core development.

Current state:
- In-house GBA core scaffold (`ingba`) with incremental ARM execution support.
- In-house DS core (`inds`) in bring-up: ARM9/ARM7 run real code on the shared CPU after a direct boot; no interrupts, IPC, or video yet. Firmware Slot-2 transition support.
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

DS firmware menu with a GBA game in Slot-2 (no `.nds` inserted). With
`bios/nds-bios/biosnds7.bin`, `biosnds9.bin`, and `dsfirmware.bin` present, the
real BIOS and firmware run; pick "Start GBA game" (tap it, or press A) and the
firmware hands the console to the GBA core:

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --interactive
```

Without those files, `ds-firmware` boot simulates the menu and switches to GBA
after two frames (`--trace-transition --frames 300` shows it headless).

Interactive controls:

- `Z` = `A`
- `X` = `B`
- `Enter` = `Start`
- `Right Shift` = `Select`
- Arrow keys = D-pad
- `A` = `L`
- `S` = `R`
- `D` = `X`, `C` = `Y` (DS)
- Left mouse button on the bottom screen = DS touchscreen
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
