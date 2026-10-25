#pragma once

#include "emugbds/arm/Bus.h"
#include "emugbds/ingba/Apu.h"
#include "emugbds/ingba/Backup.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::ingba {

class MemoryBus final : public arm::IBus {
public:
    bool LoadBios(const std::filesystem::path& bios_path, std::string& error);
    bool LoadRom(const std::filesystem::path& rom_path, std::string& error);
    bool HasBios() const noexcept { return bios_loaded_; }
    void ApplyKeyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept;
    uint16_t keyinput_state() const noexcept { return keyinput_state_; }
    void SetLcdStatus(uint16_t vcount, bool vblank, bool hblank, bool vcounter_match) noexcept;
    uint16_t PendingInterrupts() const noexcept;
    void AdvanceTimers(uint32_t cycles);
    uint32_t CyclesUntilTimerOverflow() const noexcept;
    bool ConsumeHaltRequest() noexcept {
        const bool requested = halt_requested_;
        halt_requested_ = false;
        return requested;
    }
    // True once after the VCOUNT compare target in DISPSTAT is written.
    bool ConsumeVCountTargetWrite() noexcept {
        const bool written = vcount_target_written_;
        vcount_target_written_ = false;
        return written;
    }
    void TriggerDmaTiming(uint32_t timing_code);
    void SetInterruptRequest(uint16_t flags) noexcept;
    void AcknowledgeInterruptFlags(uint16_t flags) noexcept;
    uint64_t dispcnt_write_count() const noexcept { return dispcnt_write_count_; }
    uint16_t last_dispcnt_written() const noexcept { return last_dispcnt_written_; }
    Apu& apu() noexcept { return apu_; }
    // The cartridge save chip; LoadRom picks its type and loads <rom>.sav.
    Backup& backup() noexcept { return backup_; }
    const Apu& apu() const noexcept { return apu_; }

    uint8_t Read8(uint32_t address) const override;
    uint16_t Read16(uint32_t address) const override;
    uint32_t Read32(uint32_t address) const override;
    void Write8(uint32_t address, uint8_t value) override;
    void Write16(uint32_t address, uint16_t value) override;
    void Write32(uint32_t address, uint32_t value) override;

private:
    void TryStartImmediateDma(uint32_t control_high_address, uint8_t previous_control_high);
    void ExecuteDmaTransfer(uint32_t channel, bool timed_trigger);
    void RequestSoundDma(uint32_t fifo);
    uint16_t ReadIo16Direct(uint32_t address) const;
    uint32_t ReadIo32Direct(uint32_t address) const;
    void WriteIo16Direct(uint32_t address, uint16_t value);
    void WriteIo32Direct(uint32_t address, uint32_t value);
    static bool DecodeTimerCountAddress(uint32_t address, uint32_t& timer, bool& high_byte) noexcept;
    void IncrementTimer(uint32_t timer, uint32_t increments) noexcept;
    const uint8_t* DirectReadPointer(uint32_t address, uint32_t size) const noexcept;
    // An EEPROM sits in the top of the ROM area: all of D000000h-DFFFFFFh for ROMs up to 16 MiB,
    // else DFFFF00h-DFFFFFFh (GBATEK "GBA Cart Backup EEPROM").
    bool InEepromWindow(uint32_t address) const noexcept {
        return backup_.type() == BackupType::Eeprom && (address >> 24u) == 0x0Du &&
            (rom_.size() <= 0x01000000u || address >= 0x0DFFFF00u);
    }
    static constexpr size_t kPaletteSize = 0x00000400u;
    static constexpr size_t kVramSize = 0x00018000u;
    static constexpr size_t kOamSize = 0x00000400u;
    static constexpr size_t kIoSize = 0x00000400u;

    std::vector<uint8_t> bios_;
    std::vector<uint8_t> ewram_;
    std::vector<uint8_t> iwram_;
    std::vector<uint8_t> rom_;
    // EEPROM reads shift the chip's serial output, so const reads may change it.
    mutable Backup backup_{};
    std::array<uint8_t, kPaletteSize> palette_{};
    std::array<uint8_t, kVramSize> vram_{};
    std::array<uint8_t, kOamSize> oam_{};
    std::array<uint8_t, kIoSize> io_{};
    bool bios_loaded_ = false;
    uint16_t keyinput_state_ = 0x03FFu;
    uint8_t postflg_ = 0x00u;
    uint16_t waitcnt_ = 0x4317u;
    uint64_t dispcnt_write_count_ = 0;
    uint16_t last_dispcnt_written_ = 0;
    bool halt_requested_ = false;
    bool vcount_target_written_ = false;
    std::array<uint16_t, 4u> timer_reload_{};
    std::array<uint16_t, 4u> timer_counters_{};
    std::array<uint16_t, 4u> timer_control_{};
    std::array<uint32_t, 4u> timer_prescaler_phase_{};
    std::array<uint32_t, 4u> dma_source_{};       // Internal DMA counters latched on enable.
    std::array<uint32_t, 4u> dma_destination_{};
    std::array<uint16_t, 4u> dma_count_{};
    Apu apu_{};
};

} // namespace emugbds::ingba
