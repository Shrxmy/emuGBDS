# Development Roadmap

This is the practical sequence to keep momentum while improving correctness.

## Phase 1: Stabilize In-House GBA Core

Goal: reliable boot + correct video init on more commercial ROMs.

1. CPU state model hardening
   - Implement proper ARM banked registers and mode switching semantics.
   - Validate exception entry/return paths.
2. Interrupt model
   - Correct IRQ trigger/ack timing and IF/IE/IME interaction.
   - Add deterministic tests for IRQ flow.
3. BIOS/SWI coverage
   - Expand SWI behavior accuracy for titles that still stall at startup.
4. Timer + DMA timing
   - Improve non-immediate DMA triggers and timer coupling.
5. More rendering paths
   - Beyond basic mode3/mode4 smoke, expand to BG/sprite paths incrementally.

## Phase 2: GBA Audio Correctness

Goal: stable pitch and sync.

1. Establish APU clock/timer coupling baseline.
2. Add ring buffer with fixed sample rate output.
3. Sync audio production to frame pacing in runtime.
4. Add tests for sample count consistency and basic FIFO behavior.

## Phase 3: DS Core Bring-Up (In-House)

Goal: progress from scaffold to executable ARM9/ARM7 instruction core.

1. Done: shared ARM CPU (ARMv4T/ARMv5TE) running both DS CPUs.
2. Done: ARM9/ARM7 memory maps with TCM (CP15) and WRAMCNT; direct boot.
3. Done: interrupt controllers, IPC sync/FIFO, timers, DMA, LCD timing,
   HALTCNT GBA mode as the real DS -> GBA transition.
4. In progress: boot the real BIOS and firmware (`--boot-mode bios`). Done:
   empty cartridge slot, SPI (firmware flash, power management), RTC, math
   unit, RCNT, sound register storage. The firmware now runs to its first
   screens and idles waiting for input. Next: 2D video to see the menu and
   touch/button input to pick GBA, so the firmware itself writes HALTCNT.
5. Then: 2D engines, then 3D and sound.
6. Keep Slot-2 transition support as an integration target.

## Phase 4: Runtime and Tooling

Goal: better debugging speed and confidence.

1. Add optional trace channels (`cpu`, `irq`, `dma`, `swi`, `video`).
2. Add per-frame debug snapshot dumps for reproducible bug reports.
3. Split tests into unit + integration suites for faster iteration.

## Definition of "Playable Enough" Milestone

For GBA:
- boots at least one target commercial game from intro to basic gameplay
- stable video output
- audio present with acceptable pitch and timing
- no rapid unsupported-instruction growth during first few minutes
