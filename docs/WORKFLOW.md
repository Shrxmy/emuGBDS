# Workflow Guide

## Standard Dev Loop

1. Build:

```powershell
cmake --preset msvc-release
cmake --build --preset build-msvc-release
```

2. Run tests:

```powershell
ctest --test-dir .\build -C Release --output-on-failure
```

3. Run a focused emulator command for manual verification.

## Common Run Commands

GBA headless smoke:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 300
```

GBA interactive:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --interactive
```

GBA interactive uncapped:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --interactive --uncapped
```

GBA no BIOS (direct ROM reset vector):

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --no-bios-gba --frames 600 --debug-stats
```

DS with NDS ROM:

```powershell
.\build\Release\emugbds.exe --mode ds --nds .\roms\SomeGame.nds --frames 300
```

DS firmware Slot-2 transition:

```powershell
.\build\Release\emugbds.exe --mode ds --slot2 .\roms\Fire_Emblem.gba --boot-mode ds-firmware --trace-transition --frames 300
```

## How to Debug Black Screen Quickly

Run with:

```powershell
.\build\Release\emugbds.exe --mode gba --gba .\roms\Fire_Emblem.gba --gba-core internal --frames 1200 --debug-stats
```

Check:

- `unsupported stats`
  - If non-zero and increasing fast, decoder/handler gaps exist.
- `DISPCNT` and `DISPCNT_WRITES`
  - If still `0`, the game probably never reached display init.
- `PC/SP/LR/CPSR`
  - Stable looping PC can indicate startup/IRQ/SWI flow issue.

Then inspect:

- Frame stepping, IRQ delivery, and HALT in `host/src/ingba/Core.cpp`
- ARM/Thumb execution in `host/src/arm/ArmExecute.cpp` and `ThumbExecute.cpp`
- SWI behavior in `host/src/ingba/Swi.cpp`
- CPU state and exception banking in `host/src/arm/ArmCpu.cpp`
- DMA and IO in `host/src/ingba/MemoryBus.cpp`
- Video state and composition in `host/src/ingba/Video.cpp`

## Input Mapping (Interactive)

- `Z` = A
- `X` = B
- `Enter` = Start
- `Right Shift` = Select
- Arrow keys = D-pad
- `A` = L
- `S` = R
- `Esc` = Quit

## SDL2 Runtime Note

If SDL2 is missing at runtime, place `SDL2.dll` near:

- `build/Release/emugbds.exe`

or make sure the DLL location is in `PATH`.
