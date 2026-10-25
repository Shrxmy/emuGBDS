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

Goal: an in-house DS core that plays commercial games.

1. Done: shared ARM CPU (ARMv4T/ARMv5TE) running both DS CPUs.
2. Done: ARM9/ARM7 memory maps with TCM (CP15) and WRAMCNT; direct boot.
3. Done: interrupt controllers, IPC sync/FIFO, timers, DMA, LCD timing,
   HALTCNT GBA mode as the real DS -> GBA transition.
4. Done: the real BIOS and firmware boot (ds-firmware with all NDS images):
   empty cartridge slot, SPI (firmware flash, power management, touchscreen),
   RTC, math unit, VRAM banking, both 2D engines, buttons and touch. Picking
   "Start GBA game" makes the firmware write HALTCNT and the GBA core takes over.
5. Done: commercial DS games boot to their menus (cartridge reads, save chip
   with `.sav` files, wifi stub). Shadow Dragon plays its menus and story
   scenes.
6. Done: DS sound (all channel formats, mixer, capture), paced in real time
   through the audio queue.
7. Done: CPU cycle timing on both cores; the real DS menu with both slots
   (KEY1 card boot, Slot-2 GBA start); display capture; the 3D engine
   (Devil Survivor shows its graphics). Headless: Shadow Dragon about 179 fps,
   Devil Survivor about 116 fps.
8. Next: the remaining 3D features (fog, edge marking, anti-aliasing, shadow
   polygons), 2D mosaic and the main-memory display FIFO, and save states.

## Phase 4: Runtime and Tooling

Goal: better debugging speed and confidence.

0. Done: interactive app with menus, ROM picker and drag-and-drop, pause /
   frame step / fast-forward, window scale, DS layout, fullscreen, key
   rebinding, a settings file, a DS save-chip picker, and debug panels (live
   CPU/IO state, layer toggles, screenshots, memory dumps).
1. Add optional trace channels (`cpu`, `irq`, `dma`, `swi`, `video`).
2. Add per-frame debug snapshot dumps for reproducible bug reports.
3. Split tests into unit + integration suites for faster iteration.

## Definition of "Playable Enough" Milestone

For GBA:
- boots at least one target commercial game from intro to basic gameplay
- stable video output
- audio present with acceptable pitch and timing
- no rapid unsupported-instruction growth during first few minutes
