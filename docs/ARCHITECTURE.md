# Architecture

emuGBDS is organized in layers so emulator-specific behavior does not leak into
runtime orchestration.

## Layers

```text
main.cpp
  -> RuntimeKernel
      -> IEmuCore adapters
          -> in-house GBA core (ingba)
          -> in-house DS core (inds)
```

- `host/src/main.cpp`: CLI and optional SDL presentation.
- `host/src/runtime/`: frame loop, input forwarding, and output polling.
- `host/src/cores/`: adapters implementing the common core interface.
- `host/src/ingba/`: GBA implementation.
- `host/src/inds/`: DS implementation and current bring-up scaffold.
- `host/src/boot/`, `host/src/gba/`, and `host/src/nds/`: boot policy and
  cartridge metadata, independent of CPU execution.

## In-house GBA module boundaries

| Module | Owns |
| --- | --- |
| `Core.cpp` | reset, frame stepping, instruction dispatch, IRQ/HALT orchestration |
| `CpuState.cpp` | CPU modes, banked registers, CPSR/SPSR, exceptions, diagnostics counters |
| `ArmExecute.cpp` | ARM instruction families |
| `ThumbExecute.cpp` | Thumb instruction families |
| `Swi.cpp` | BIOS/SWI behavior |
| `MemoryBus.cpp` | address map, MMIO, timers, DMA, cartridge save device |
| `Video.cpp` | LCD timing and video composition |
| `Apu.cpp` | Direct Sound FIFOs and 32768 Hz stereo mixing (PSG channels not yet implemented) |
| `*Constants.h` | focused internal constants/helpers; not public API |

New behavior should be placed in the narrowest owning module. `Core.cpp` should
coordinate modules rather than grow new device implementations.

## Build ownership

`EMUGBDS_INHOUSE_GBA_SOURCES` in the root `CMakeLists.txt` is the single source
of truth for GBA implementation units. Both the application and unit-test
executables consume that list to prevent source drift.

## Workspace policy

The following are intentionally local and ignored:

- `build*` and other generated build trees
- `bios/` and `roms/`
- `basis/` and other clean-room reference archives
- IDE metadata and compiler output

Only project-owned source, headers, tests, documentation, and build metadata
belong in version control. Empty placeholder directories are not retained;
create a directory when it has an owning module or a real file to put there.
