# emuGBDS

Clean-room emulator host focused on in-house core development.

Version 0.3.0 (`emugbds --version`; set in `CMakeLists.txt` `project()`).

Current state:
- In-house GBA core scaffold (`ingba`) with incremental ARM execution support.
- In-house DS core (`inds`) in bring-up: both CPUs with cycle timing, 2D and 3D video, display capture, input, cartridge and save chip, sound, and the real BIOS/firmware: the DS menu boots the Slot-1 card or the Slot-2 GBA cartridge.
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
GBA and DS games save to `<rom>.sav` next to the ROM (GBA: SRAM, FLASH, or
EEPROM, picked from the ROM). Settings are kept in `emugbds.ini` next to the executable; screenshots (BMP)
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

The real DS menu, with a card in Slot-1 and a GBA cartridge in Slot-2 (needs the DS BIOS
files and firmware; in the app: File > Slot-1 card, File > Slot-2 cartridge, File > Start DS menu):

```powershell
.\build\Release\emugbds.exe --ds-menu --nds .\roms\Fire_Emblem_Shadow_Dragon.nds --slot2 .\roms\Fire_Emblem.gba
```

The 3D engine does not do fog, edge marking, anti-aliasing, or shadow polygons yet.

DS firmware menu with a GBA game in Slot-2 (no `.nds` inserted). With
`bios/nds-bios/biosnds7.bin`, `biosnds9.bin`, and `dsfirmware.bin` present, the
real BIOS and firmware run; pick "Start GBA game" (tap it, or press A) and the
firmware hands the console to the GBA core:

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --interactive
```

Without those files, `ds-firmware` boot simulates the menu and switches to GBA
after two frames (`--trace-transition --frames 300` shows it headless).

Settings you change in the DS menu (name, birthday, language, touch
calibration...) are kept in `dsfirmware.sav` next to your firmware dump, which
is loaded instead of the dump from then on; the dump itself is never modified.

Both slots: a DS game can have a GBA cartridge in Slot-2 (File > Slot-2
cartridge, remembered per DS game, or `--nds game.nds --slot2 game.gba`). The DS
game sees the cartridge's ROM and its SRAM/FLASH save (the same `<gba rom>.sav`
GBA mode uses), as games like Pokemon Diamond/Pearl/Platinum need for Pal Park.

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
- `Esc` = leave fullscreen, or exit when windowed
- Gamepad (SDL game controllers, plugged in any time): face buttons by
  position as on a Nintendo pad (right = A, bottom = B, top = X, left = Y),
  D-pad or left stick, shoulders = L/R, Back/Start = Select/Start, right
  trigger = fast-forward

Keyboard keys and gamepad buttons can be changed in Settings > Controls.
Hotkeys: `Ctrl+O` open, `Ctrl+R` reset, `P` pause, `N` step one frame (while
paused), hold `Tab` to fast-forward, `M` mute, `F1` CPU/IO panel, `F11`
fullscreen, `F12` screenshot. Settings also has a volume slider and a "pause
when the window loses focus" option. BIOS files are found under `bios\` in the
working folder, next to the executable, or two folders above it.

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

## License

emuGBDS is released under the MIT License (see `LICENSE`). Bundled third-party
code (SDL, Dear ImGui) keeps its own license; see `THIRD_PARTY_NOTICES.md`.
No BIOS, firmware, or game files are included or distributed.

## Privacy

`roms/` and `bios/` are intended for local development assets and should not be committed.
