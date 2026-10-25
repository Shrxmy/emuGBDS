# Codebase Tour

## 1) Entry Point

- File: `host/src/main.cpp`
- Responsibilities:
  - Parse CLI arguments (`--mode`, `--gba`, `--nds`, `--slot2`, `--interactive`, `--debug-stats`, etc.)
  - Without `--frames`, hand over to the interactive app (`host/src/frontend/App.cpp`)
  - With `--frames N`, start a `frontend::Session` and run N frames headless
  - Print transition/debug output

The interactive app (`host/src/frontend/`) owns the SDL window and Dear ImGui
menus. Opening a ROM builds a new `Session` (`Session.h`) from a `BootConfig`;
the previous session is destroyed first so its DS save is written before the
new one reads it. Settings live in `emugbds.ini` next to the executable
(`Settings.h`, unit-tested in `frontend_settings`). Debug hooks it uses on the
cores: `set_hidden_layers` (GBA `ingba::Core`, DS `Engine2d`) and
`inds::MemoryBus::DebugRead8`.

## 2) Runtime Layer (Mode Orchestration)

- Files:
  - `host/include/emugbds/runtime/RuntimeKernel.h`
  - `host/src/runtime/RuntimeKernel.cpp`
  - `host/include/emugbds/runtime/Types.h`
- Responsibilities:
  - Boot selected mode (DS/GBA)
  - Tick one frame at a time
  - Forward inputs to active core
  - Expose video/audio output bundle
  - Handle DS -> GBA transition events

Think of this as the host "OS" for emulator cores.

## 3) Core Interface

- Files:
  - `host/include/emugbds/core/IEmuCore.h`
  - `host/include/emugbds/core/Types.h`
- Responsibilities:
  - Define common contract for any core adapter
  - Keep DS and GBA plugged into one runtime without core-specific logic in host

## 4) GBA Path (In-House Core)

- Adapter:
  - `host/include/emugbds/cores/InHouseGbaCoreAdapter.h`
  - `host/src/cores/InHouseGbaCoreAdapter.cpp`
- Core API and orchestration:
  - `host/include/emugbds/ingba/Core.h`
  - `host/src/ingba/Core.cpp`
- Shared ARM CPU (state, bank switching, instruction execution):
  - `host/include/emugbds/arm/ArmCpu.h`, `CpuState.h`, `Bus.h`
  - `host/src/arm/ArmCpu.cpp`
  - `host/src/arm/ArmExecute.cpp`
  - `host/src/arm/ThumbExecute.cpp`
- BIOS/SWI emulation:
  - `host/src/ingba/Swi.cpp`
- Memory bus, MMIO, timers, DMA, and Flash:
  - `host/include/emugbds/ingba/MemoryBus.h`
  - `host/src/ingba/MemoryBus.cpp`
- Audio:
  - `host/include/emugbds/ingba/Apu.h`
  - `host/src/ingba/Apu.cpp`
- Video composition:
  - `host/src/ingba/Video.cpp`
  - `host/include/emugbds/ingba/VideoConstants.h`
- Internal constants and diagnostics:
  - `host/include/emugbds/ingba/MemoryConstants.h`
  - `host/include/emugbds/arm/CpuConstants.h`
  - `host/include/emugbds/arm/DebugTrace.h`

### Where to edit for GBA features

- Frame stepping, IRQ delivery, and HALT: `host/src/ingba/Core.cpp`
- ARM/Thumb instruction behavior: the corresponding unit in `host/src/arm/`
- CPU modes, banked registers, and exception state: `host/src/arm/ArmCpu.cpp`
- RAM/VRAM/IO/DMA/save behavior: `MemoryBus.cpp`
- Backgrounds, sprites, windows, blending, and pixels: `Video.cpp`
- Input key state plumbing: adapter + `Core::ApplyInputMasks`

The implementation units are shared by the application and test targets through
`EMUGBDS_INHOUSE_GBA_SOURCES` in the root `CMakeLists.txt`; keep that list as
the single source-of-truth when adding a new GBA module.

## 5) DS Path (In-House Core)

- Adapter:
  - `host/include/emugbds/cores/InHouseDsCoreAdapter.h`
  - `host/src/cores/InHouseDsCoreAdapter.cpp`
- Core (two shared `arm::ArmCpu` instances, scheduler, direct boot):
  - `host/include/emugbds/inds/Core.h`
  - `host/src/inds/Core.cpp`
- Memory (ARM9/ARM7 bus views, TCM, shared WRAM, I/O, unmapped-access log):
  - `host/include/emugbds/inds/MemoryBus.h`
  - `host/src/inds/MemoryBus.cpp`
- ARM9 CP15 (control register, TCM regions, wait for interrupt):
  - `host/include/emugbds/inds/Cp15.h`
  - `host/src/inds/Cp15.cpp`
- I/O, IPC, timers, and devices:
  - `host/include/emugbds/inds/Io.h`, `Ipc.h`, `Timers.h`, `GameCard.h`, `Spi.h`, `Rtc.h`, `MathUnit.h`
  - `host/src/inds/Io.cpp`, `Ipc.cpp`, `Timers.cpp`, `GameCard.cpp`, `Spi.cpp`, `Rtc.cpp`, `MathUnit.cpp`
- Video: `VideoMemory.h/.cpp` (VRAM banks, palettes, OAM), `Engine2d.h/.cpp` (2D engines)

Current DS core status:
- ARM9 (ARMv5TE) and ARM7 (ARMv4T) execute real code, interleaved 2:1.
- A loaded `.nds` is direct-booted: header copied to 027FFE00h, both binaries
  loaded, both CPUs started at their entry points; SWIs run the BIOS images.
- ARM9/ARM7 memory maps with ITCM/DTCM and WRAMCNT.
- Interrupts (IME/IE/IF, halt and wake), IPC sync/FIFO, timers, DMA
  (immediate, V-Blank, H-Blank), DISPSTAT/VCOUNT line timing.
- An ARM7 HALTCNT GBA-mode write switches the console to the GBA core.
- Empty cartridge slot, SPI firmware flash/power management, RTC, math unit.
- Unemulated I/O, IME/IE/IF and halt state are listed by `--debug-stats` in DS mode.
- Without a cartridge, ds-firmware boot runs the real BIOS and firmware when
  all three images are present: the menu renders, takes buttons and touch, and
  "Start GBA game" hands off to the GBA core through HALTCNT. Without the
  images it simulates the Slot-2 pick.
- VRAM banking, palettes, OAM, and both 2D engines (no 3D, mosaic, or capture).
- Direct-booted cartridges read their ROM through the card bus (B7h), keep saves
  in `<rom>.sav` next to the ROM, and find a stubbed wifi chip and an idle 3D
  geometry engine. Fire Emblem: Shadow Dragon reaches its menus and story;
  games that draw with the 3D engine (e.g. Devil Survivor's main screen) show
  nothing in that layer yet.

## 6) Boot Policy (Default BIOS Paths)

- Files:
  - `host/include/emugbds/boot/BootPolicy.h`
  - `host/src/boot/BootPolicy.cpp`
- Responsibilities:
  - Auto-detect local BIOS files from:
    - `bios/gb-bios/gba_bios.bin`
    - `bios/nds-bios/biosnds7.bin`
    - `bios/nds-bios/biosnds9.bin`
    - `bios/nds-bios/dsfirmware.bin`

## 7) Cartridge Header Parsing

- GBA: `host/include/emugbds/gba/GbaCartridgeHeader.h`, `host/src/gba/GbaCartridgeHeader.cpp`
- NDS: `host/include/emugbds/nds/NdsCartridgeHeader.h`, `host/src/nds/NdsCartridgeHeader.cpp`

Used to validate ROM basics and boot metadata.

## 8) Tests

- File: `tests/TestMain.cpp`
- Contains:
  - In-house GBA memory/CPU/video tests
  - In-house DS memory/transition scaffold tests
  - Runtime kernel integration tests

When adding a feature, add or extend at least one test here.
