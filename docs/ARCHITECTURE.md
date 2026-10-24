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
- `host/src/inds/`: DS implementation in bring-up (dual CPUs, memory map, CP15).
- `host/src/boot/`, `host/src/gba/`, and `host/src/nds/`: boot policy and
  cartridge metadata, independent of CPU execution.

## Shared ARM CPU (`arm`)

| Module | Owns |
| --- | --- |
| `ArmCpu.cpp` | instruction dispatch, CPU modes, banked registers, CPSR/SPSR, exception entry, diagnostics counters |
| `ArmExecute.cpp` | ARM instruction families |
| `ThumbExecute.cpp` | Thumb instruction families |

`ArmCpu` reaches memory only through `arm::IBus` and asks its system for
SWI handling and coprocessor transfers (MRC/MCR) through `arm::ICpuHost`.
Interrupt delivery is the system's job: it decides when an IRQ is taken and
calls `ArmCpu::EnterException`. Keep system-specific behavior (memory maps,
BIOS emulation, IRQ registers, CP15 state) out of this module.

Each CPU is built for one `arm::ArmArchitecture`: `V4T` (ARM7TDMI: GBA, DS
ARM7) or `V5TE` (ARM946E-S: DS ARM9). V5TE adds BLX, CLZ, the Q-saturating
ALU ops, signed halfword multiplies, LDRD/STRD, PLD, MRC/MCR, and the ARMv5
rules for loads into PC (interworking) and for LDM/STM with the base in the
register list. Every ARMv5 behavior is gated on the architecture, so ARMv4
decoding is unaffected.

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

## In-house DS module boundaries

| Module | Owns |
| --- | --- |
| `Core.cpp` | ARM9/ARM7 `ArmCpu` instances and their hosts (BIOS SWI vectors, CP15), the 2:1 scheduler, IRQ delivery and halt/wake, LCD line timing, direct boot, the HALTCNT GBA-mode transition |
| `MemoryBus.cpp` | ARM9/ARM7 bus views, RAM/TCM/WRAM mapping, DMA access (no TCM), routing I/O to `Io`, the unmapped-access log |
| `Io.cpp` | per-CPU I/O registers: IME/IE/IF, DISPSTAT/VCOUNT, DMA, keypad, POSTFLG, HALTCNT, POWCNT, EXMEMCNT, VRAMCNT/WRAMCNT |
| `Ipc.cpp` | IPCSYNC and the two IPC FIFOs with their edge-triggered IRQs |
| `Timers.cpp` | one CPU's four 33.51 MHz timers with cascade and overflow IRQs |
| `GameCard.cpp` | Slot-1 card bus registers, card DMA and transfer IRQ (empty slot only) |
| `Spi.cpp` | ARM7 SPI bus: firmware flash (in-memory copy), power management, touchscreen stub |
| `Rtc.cpp` | ARM7 bit-banged Seiko RTC on 4000138h, host clock |
| `MathUnit.cpp` | ARM9 divider and square root |
| `Cp15.cpp` | ARM9 CP15: ID and control registers, TCM regions, wait for interrupt |

The ARM9's CP15 is owned by the memory object because its TCM settings decide
the ARM9 memory map. I/O is dispatched per 32-bit register word with a byte-lane
mask, so side effects (FIFO pops) happen once per access. A loaded cartridge is
always direct-booted; the firmware menu is not emulated. When the ARM7 writes
HALTCNT power-down mode 1 (GBA mode), the DS core raises the DS -> GBA
transition, the same event the runtime uses for Slot-2 boot.

## Build ownership

`EMUGBDS_ARM_CPU_SOURCES`, `EMUGBDS_INHOUSE_GBA_SOURCES`, and
`EMUGBDS_INHOUSE_DS_SOURCES` in the root `CMakeLists.txt` are the single source
of truth for CPU, GBA, and DS implementation units. Both the application and unit-test
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
