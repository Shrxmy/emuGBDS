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
- `host/src/arm/`: shared ARM CPU interpreter, used by the GBA core and
  intended for the DS ARM7/ARM9.
- `host/src/ingba/`: GBA implementation.
- `host/src/inds/`: DS implementation and current bring-up scaffold.
- `host/src/boot/`, `host/src/gba/`, and `host/src/nds/`: boot policy and
  cartridge metadata, independent of CPU execution.

## Shared ARM CPU (`arm`)

| Module | Owns |
| --- | --- |
| `ArmCpu.cpp` | instruction dispatch, CPU modes, banked registers, CPSR/SPSR, exception entry, diagnostics counters |
| `ArmExecute.cpp` | ARM instruction families |
| `ThumbExecute.cpp` | Thumb instruction families |

`ArmCpu` reaches memory only through `arm::IBus` and asks its system for
SWI handling through `arm::ICpuHost`. Interrupt delivery is the system's job:
it decides when an IRQ is taken and calls `ArmCpu::EnterException`. Keep
system-specific behavior (memory maps, BIOS emulation, IRQ registers) out of
this module.

## In-house GBA module boundaries

| Module | Owns |
| --- | --- |
| `Core.cpp` | reset, frame stepping, IRQ/HALT orchestration, driving the shared `ArmCpu` |
| `Swi.cpp` | BIOS/SWI behavior (the `ICpuHost` implementation) |
| `MemoryBus.cpp` | address map, MMIO, timers, DMA, cartridge save device |
| `Video.cpp` | LCD timing and video composition |
| `Apu.cpp` | Direct Sound FIFOs and 32768 Hz stereo mixing (PSG channels not yet implemented) |
| `*Constants.h` | focused internal constants/helpers; not public API |

New behavior should be placed in the narrowest owning module. `Core.cpp` should
coordinate modules rather than grow new device implementations.

## Build ownership

`EMUGBDS_ARM_CPU_SOURCES` and `EMUGBDS_INHOUSE_GBA_SOURCES` (which includes
the CPU list) in the root `CMakeLists.txt` are the single source of truth for
CPU and GBA implementation units. Both the application and unit-test
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
