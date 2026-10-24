#pragma once

// DS I/O ports shared by MemoryBus and the scheduler: per-CPU interrupt
// controllers, DISPSTAT/VCOUNT, DMA, timers, keypad, IPC, memory/power control,
// POSTFLG, and HALTCNT (whose GBA-mode setting hands the console to the GBA core).
// Registers are accessed per 32-bit word with a byte-lane mask; CPU index 0 is
// the ARM9 and 1 is the ARM7.

#include "emugbds/inds/GameCard.h"
#include "emugbds/inds/Ipc.h"
#include "emugbds/inds/MathUnit.h"
#include "emugbds/inds/Rtc.h"
#include "emugbds/inds/Spi.h"
#include "emugbds/inds/Timers.h"

#include <array>
#include <cstdint>

namespace emugbds::inds {

class MemoryBus;

class Io {
public:
    static constexpr uint32_t kIrqVBlank = 1u << 0;
    static constexpr uint32_t kIrqHBlank = 1u << 1;
    static constexpr uint32_t kIrqVCount = 1u << 2;
    static constexpr uint16_t kVisibleLines = 192u;
    static constexpr uint16_t kTotalLines = 263u;

    explicit Io(MemoryBus& memory) noexcept : memory_(memory) {}
    Io(const Io&) = delete;
    Io& operator=(const Io&) = delete;

    void Reset();
    // Return false for registers that are not emulated yet.
    bool ReadWord(int cpu, uint32_t address, uint32_t mask, uint32_t& value);
    bool WriteWord(int cpu, uint32_t address, uint32_t value, uint32_t mask);

    // Scheduler hooks.
    void AdvanceTimers(uint32_t cycles);
    void StartLine(uint16_t vcount);
    void StartHBlank();

    // IME && (IE & IF): the CPU takes an IRQ unless CPSR.I is set.
    bool IrqAsserted(int cpu) const noexcept {
        return (cpus_[cpu].ime & 1u) != 0u && (cpus_[cpu].ie & cpus_[cpu].iflags) != 0u;
    }
    // Halt release: any enabled request (the ARM9's CP15 halt also needs IME).
    bool WakeRequested(int cpu) const noexcept {
        return (cpus_[cpu].ie & cpus_[cpu].iflags) != 0u && (cpu == 1 || (cpus_[cpu].ime & 1u) != 0u);
    }
    void RequestIrq(int cpu, uint32_t bits) noexcept { cpus_[cpu].iflags |= bits; }
    bool ConsumeArm7HaltRequest() noexcept { return Consume(arm7_halt_requested_); }
    // The ARM7 wrote HALTCNT power-down mode 1: switch the console to GBA mode.
    bool gba_mode_requested() const noexcept { return gba_mode_requested_; }
    bool ConsumeGbaModeRequest() noexcept { return Consume(gba_mode_requested_); }
    uint16_t vcount() const noexcept { return vcount_; }
    uint16_t powcnt(int cpu) const noexcept { return cpus_[cpu].powcnt; }
    // Held buttons as input::GbaKey bits (A..L in bits 0-9, X in 10, Y in 11).
    void SetKeys(uint32_t held) noexcept { held_keys_ = held; }
    void SetTouch(bool down, uint16_t adc_x, uint16_t adc_y) noexcept {
        pen_down_ = down;
        spi_.SetTouch(down, adc_x, adc_y);
    }
    // Interrupt registers, for bring-up reports.
    uint32_t ime(int cpu) const noexcept { return cpus_[cpu].ime; }
    uint32_t ie(int cpu) const noexcept { return cpus_[cpu].ie; }
    uint32_t iflags(int cpu) const noexcept { return cpus_[cpu].iflags; }

private:
    struct DmaChannel {
        uint32_t source = 0;      // SAD register
        uint32_t destination = 0; // DAD register
        uint32_t control = 0;     // DMACNT: word count (low bits) and control (high bits)
        uint32_t internal_source = 0;
        uint32_t internal_destination = 0;
        uint32_t internal_count = 0;
    };

    struct CpuIo {
        uint32_t ime = 0;
        uint32_t ie = 0;
        uint32_t iflags = 0;
        uint16_t dispstat = 0;
        uint16_t keycnt = 0;
        uint8_t postflg = 0;
        uint16_t exmem_local = 0; // EXMEMCNT bits 0-6 are per CPU
        uint16_t powcnt = 0;
        Timers timers{};
        std::array<DmaChannel, 4> dma{};
    };

    static bool Consume(bool& flag) noexcept {
        const bool value = flag;
        flag = false;
        return value;
    }
    static uint32_t Merge(uint32_t old_value, uint32_t value, uint32_t mask) noexcept {
        return (old_value & ~mask) | (value & mask);
    }
    void CollectIpcIrqs() noexcept;
    uint32_t DmaStartMode(int cpu, uint32_t control) const noexcept;
    void WriteDma(int cpu, uint32_t channel, uint32_t field, uint32_t value, uint32_t mask);
    void RunDma(int cpu, uint32_t channel);
    void TriggerDma(int cpu, uint32_t mode);
    void UpdateVCountMatch(int cpu) noexcept;
    // The CPU that owns the NDS slot (EXMEMCNT bit 11).
    int CardOwner() const noexcept { return (exmem_shared_ & (1u << 11)) != 0u ? 1 : 0; }
    void PumpCardDma();
    uint16_t ExtKeys() const noexcept;
    void RaiseCardIrq() noexcept;

    MemoryBus& memory_;
    std::array<CpuIo, 2> cpus_{};
    Ipc ipc_{};
    GameCard card_{};
    Spi spi_{};
    MathUnit math_{};
    Rtc rtc_{};
    // ARM7 sound registers 400h-51Fh.
    // shortcut: plain storage; channels and capture finish at once (busy bits clear) until DS audio exists.
    std::array<uint32_t, 0x48> sound_{};
    std::array<uint32_t, 4> dma_fill_{};      // ARM9 DMA fill data (40000E0h-40000EFh)
    uint16_t exmem_shared_ = 0;               // EXMEMCNT bits 7-15, set by the ARM9 only
    uint16_t wifiwaitcnt_ = 0;
    uint16_t rcnt_ = 0; // ARM7 RCNT, stored only (no serial port)
    uint32_t held_keys_ = 0;
    bool pen_down_ = false;
    uint16_t vcount_ = 0;
    bool arm7_halt_requested_ = false;
    bool gba_mode_requested_ = false;
};

} // namespace emugbds::inds
