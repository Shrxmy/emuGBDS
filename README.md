# emuGBDS

Clean-room emulator host focused on in-house core development.

Current state:
- In-house GBA core scaffold (`ingba`) with incremental ARM execution support.
- In-house DS core (`inds`) in bring-up: both CPUs, 2D video, input, cartridge and save chip, and the real BIOS/firmware handoff to GBA; no 3D or sound yet.
- SDL2 + Dear ImGui app with menus, a ROM picker, settings, and debug panels.
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

Open the app and pick a game with File > Open ROM (Ctrl+O), or drag a `.gba` /
`.nds` file onto the window:

```powershell
.\build\Release\emugbds.exe
```

Or start a game straight away (the window opens unless `--frames` is given):

```powershell
.\build\Release\emugbds.exe .\roms\Fire_Emblem.gba
.\build\Release\emugbds.exe .\roms\Fire_Emblem_Shadow_Dragon.nds
```

The menus cover pause / frame step / fast-forward, window size, DS screen
layout, fullscreen, BIOS paths, key bindings, the DS save chip type, and debug
panels (live CPU/IO state, BG/OBJ layer toggles, screenshots, memory dumps).
Settings are kept in `emugbds.ini` next to the executable; screenshots (BMP)
and memory dumps go to `screenshots\` and `dumps\` there.

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

In-house DS game (direct boot; saves go to `<rom>.sav` next to the ROM):

```powershell
.\build\Release\emugbds.exe --mode ds --nds .\roms\Fire_Emblem_Shadow_Dragon.nds --interactive
```

DS games that draw with the 3D engine show nothing in that layer yet.

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

Game keys can be changed in Settings > Controls. Hotkeys: `Ctrl+O` open,
`Ctrl+R` reset, `P` pause, `N` step one frame (while paused), hold `Tab` to
fast-forward, `F1` CPU/IO panel, `F11` fullscreen, `F12` screenshot.

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
