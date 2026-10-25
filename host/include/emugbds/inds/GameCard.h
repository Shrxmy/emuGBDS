#pragma once

// NDS-slot (Slot-1) cartridge bus: AUXSPICNT/AUXSPIDATA (40001A0h), ROMCTRL
// (40001A4h), the 8-byte command (40001A8h), KEY2 seeds (40001B0h-40001BBh), and
// the data-in port (4100010h). Shared by both CPUs; EXMEMCNT decides which owns it.
// The inserted cartridge answers commands as seen by the CPU after KEY2 decoding
// (the bus encrypts and the card decrypts, so software only sees plain data).
// shortcut: no KEY1 stage, so the real BIOS cannot boot an inserted card yet
// (direct boot is used instead); an empty ROM image is an empty slot.

#include "emugbds/inds/Backup.h"

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::inds {

class GameCard {
public:
    static constexpr uint32_t kTransferIrq = 1u << 19;

    // The cartridge ROM image (kept across Reset); empty = no card inserted.
    void AttachRom(const std::vector<uint8_t>* rom) noexcept { rom_ = rom; }
    // Chip ID as returned by commands 90h/B8h: Macronix, (size in MiB) - 1.
    uint32_t ChipId() const noexcept;
    void Reset() noexcept;
    Backup& backup() noexcept { return backup_; }
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

    enum class Source : uint8_t { HighZ, Zero, Header, ChipId, Rom };

    void StartTransfer() noexcept;
    void FinishTransfer() noexcept;
    uint32_t NextWord() noexcept;
    uint8_t RomByte(uint32_t address) const noexcept;

    uint16_t auxspicnt_ = 0;
    uint8_t auxspidata_ = 0;
    uint32_t romctrl_ = 0;
    std::array<uint32_t, 2> command_{};
    std::array<uint32_t, 3> seeds_{}; // write-only KEY2 seed registers, kept for completeness
    uint32_t words_remaining_ = 0;
    bool transfer_irq_ = false;
    const std::vector<uint8_t>* rom_ = nullptr;
    Backup backup_{}; // kept across Reset
    Source source_ = Source::HighZ;
    uint32_t address_ = 0; // Start address of a header/ROM read
    uint32_t offset_ = 0;  // Bytes delivered so far
    uint32_t mirror_size_ = 0x8000u; // ROM size rounded up to a power of two
};

} // namespace emugbds::inds
