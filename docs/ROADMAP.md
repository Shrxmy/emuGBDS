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

1. Replace PC-advance scaffold with real fetch/decode/execute loop.
2. Build DS memory map step-by-step (RAM, WRAM banking, MMIO stubs).
3. Implement BIOS boot flow and minimal firmware behavior.
4. Keep Slot-2 transition support as an integration target.

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
