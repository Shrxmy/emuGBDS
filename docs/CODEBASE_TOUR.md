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
- Core API and orchestration:
  - `host/include/emugbds/ingba/Core.h`
  - `host/src/ingba/Core.cpp`
- CPU state and bank switching:
  - `host/include/emugbds/ingba/CpuState.h`
  - `host/src/ingba/CpuState.cpp`
- Memory bus, MMIO, timers, DMA, and Flash:
  - `host/include/emugbds/ingba/MemoryBus.h`
  - `host/src/ingba/MemoryBus.cpp`
- Instruction execution:
  - `host/src/ingba/ArmExecute.cpp`
  - `host/src/ingba/ThumbExecute.cpp`
  - `host/src/ingba/Swi.cpp`
- Video composition:
  - `host/src/ingba/Video.cpp`
  - `host/include/emugbds/ingba/VideoConstants.h`
- Internal constants and diagnostics:
  - `host/include/emugbds/ingba/MemoryConstants.h`
  - `host/include/emugbds/ingba/CpuConstants.h`
  - `host/include/emugbds/ingba/DebugTrace.h`

### Where to edit for GBA features

- CPU lifecycle, frame stepping, and dispatch: `host/src/ingba/Core.cpp`
- ARM/Thumb instruction behavior: the corresponding execution unit above
- CPU modes, banked registers, and exception state: `CpuState.cpp`
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
