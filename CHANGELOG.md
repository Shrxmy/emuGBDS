# Changelog

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
