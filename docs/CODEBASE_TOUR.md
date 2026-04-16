# Codebase Tour

## 1) Entry Point

- File: `host/src/main.cpp`
- Responsibilities:
  - Parse CLI arguments (`--mode`, `--gba`, `--nds`, `--slot2`, `--interactive`, `--debug-stats`, etc.)
  - Build `RuntimeKernel` with DS and GBA adapters
  - Drive frame loop (headless or SDL interactive)
  - Print transition/debug output

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
- Core:
  - `host/include/emugbds/ingba/Core.h`
  - `host/src/ingba/Core.cpp`
  - `host/include/emugbds/ingba/MemoryBus.h`

### Where to edit for GBA features

- CPU decode/execute (ARM/Thumb/SWI): `host/src/ingba/Core.cpp`
- Memory and IO behavior (RAM/VRAM/IO/DMA): `host/src/ingba/Core.cpp` (MemoryBus section)
- Video output generation: `UpdateVideoFrame` path in `host/src/ingba/Core.cpp`
- Input key state plumbing: adapter + `ApplyInputMasks` in core

## 5) DS Path (In-House Core)

- Adapter:
  - `host/include/emugbds/cores/InHouseDsCoreAdapter.h`
  - `host/src/cores/InHouseDsCoreAdapter.cpp`
- Core:
  - `host/include/emugbds/inds/Core.h`
  - `host/src/inds/Core.cpp`
  - `host/include/emugbds/inds/MemoryBus.h`

Current DS core status:
- Frame/timing scaffold for ARM9/ARM7
- BIOS/firmware/ROM loading
- Slot-2 ROM loading and transition signaling
- Not yet a full playable DS implementation

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
