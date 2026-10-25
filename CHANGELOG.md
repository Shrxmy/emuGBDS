# Changelog

## 0.3.0

DS
- 3D engine: geometry commands and geometry DMA, matrices, lighting, clipping, textures in all
  formats, toon/highlight shading, translucency, and the rear plane, shown as BG0 and usable by
  display capture. Games that draw with 3D (e.g. Devil Survivor) now show their graphics. Not
  yet: fog, edge marking, anti-aliasing, and shadow polygons.
- Display capture: games that swap screens every frame to fill both screens no longer strobe.
- The real DS menu with both slots: File > Slot-1 card and Slot-2 cartridge pick the
  cartridges, and File > Start DS menu boots the menu, which starts either one (the card goes
  through the cartridge's KEY1 encryption stage, as on hardware). Also `--ds-menu`. An empty
  Slot-2 now reads as empty instead of as a DS Option Pak.
- A GBA cartridge can sit in Slot-2 and keeps its save; firmware settings are saved to
  `<firmware>.sav`.
- Fixed a crash when the DS menu starts the Slot-2 GBA game.

GBA and DS
- CPU timing: instructions take their real cycle counts (memory waitstates, WAITCNT, multiplies,
  branches) instead of one cycle each.
- Faster: DS 2D rendering and the CPUs' memory access; on CPUs with performance and efficiency
  cores the emulator stays on the performance cores. Busy DS menu transitions no longer stall.
- Lower memory use: exact-size ROM loads, and each core is allocated only when used.

App
- Application icon (`assets/branding`).

## 0.2.0

First public build. Windows x64; no BIOS, firmware, or game files are included.

GBA
- Cartridge saves: SRAM/FRAM, FLASH 64K/128K, and EEPROM, picked from the ROM and
  written to `<rom>.sav` next to the ROM (Fire Emblem can now save).
- Cartridge real-time clock: Pokemon Ruby/Sapphire/Emerald no longer report a
  dead battery; the clock follows the PC's time.
- Full sound: the four PSG channels (square, wave, noise) join Direct Sound.

DS
- Sound: all 16 channels (PCM8/PCM16/IMA-ADPCM, square, noise), mixer, and capture.
- About 3x faster 2D rendering; games run above full speed on typical PCs.
- 3D is not emulated yet: games that draw with it (e.g. Devil Survivor) show blank
  layers.

App
- Gamepad support (hot-plug, rebindable), volume slider and mute (`M`).
- `Esc` leaves fullscreen before it quits; optional pause when unfocused.
- BIOS files are also found in `bios\` next to the executable.

Project
- Licensed under MIT.

## 0.1.0

- In-house GBA core with video, Direct Sound audio, and input.
- In-house DS core: both CPUs, 2D video, input, cartridge and save chip, and the
  real BIOS/firmware handoff to GBA mode.
- SDL2 + Dear ImGui app: ROM picker and drag-and-drop, pause/step/fast-forward,
  window scale and DS layout, settings, and debug panels.
