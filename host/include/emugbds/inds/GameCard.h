#pragma once

// NDS-slot (Slot-1) cartridge bus: AUXSPICNT/AUXSPIDATA (40001A0h), ROMCTRL
// (40001A4h), the 8-byte command (40001A8h), KEY2 seeds (40001B0h-40001BBh), and
// the data-in port (4100010h). Shared by both CPUs; EXMEMCNT decides which owns it.
// shortcut: models an empty slot only (data lines read high); a real cartridge
// protocol with KEY1/KEY2 encryption is needed to BIOS-boot an inserted card.

#include <array>
#include <cstdint>

namespace emugbds::inds {

class GameCard {
public:
    static constexpr uint32_t kTransferIrq = 1u << 19;

    void Reset() noexcept;
    // offset = address - 40001A0h for the 40001A0h-40001BBh register block.
    uint32_t ReadWord(uint32_t offset) const noexcept;
    void WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept;
    bool data_ready() const noexcept { return (romctrl_ & kDataReady) != 0u; }
    uint32_t words_remaining() const noexcept { return words_remaining_; }
    // Reads the next 4-byte data word (4100010h).
    uint32_t ReadData() noexcept;
    // True once after a transfer finished with its IRQ enabled in AUXSPICNT.
    bool ConsumeTransferIrq() noexcept {
        const bool raised = transfer_irq_;
        transfer_irq_ = false;
        return raised;
    }

private:
    static constexpr uint32_t kDataReady = 1u << 23;
    static constexpr uint32_t kBusy = 1u << 31;

    void StartTransfer() noexcept;
    void FinishTransfer() noexcept;

    uint16_t auxspicnt_ = 0;
    uint8_t auxspidata_ = 0;
    uint32_t romctrl_ = 0;
    std::array<uint32_t, 2> command_{};
    std::array<uint32_t, 3> seeds_{}; // write-only KEY2 seed registers, kept for completeness
    uint32_t words_remaining_ = 0;
    bool transfer_irq_ = false;
};

} // namespace emugbds::inds
