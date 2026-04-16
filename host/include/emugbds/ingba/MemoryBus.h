#pragma once

#include "emugbds/ingba/Bus.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::ingba {

class MemoryBus final : public IBus {
public:
    bool LoadBios(const std::filesystem::path& bios_path, std::string& error);
    bool LoadRom(const std::filesystem::path& rom_path, std::string& error);
    bool HasBios() const noexcept { return bios_loaded_; }
    void ApplyKeyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept;
    uint16_t keyinput_state() const noexcept { return keyinput_state_; }
    void SetLcdStatus(uint16_t vcount, bool vblank, bool hblank) noexcept;
    void TriggerDmaTiming(uint32_t timing_code);
    void SetInterruptRequest(uint16_t flags) noexcept;
    uint64_t dispcnt_write_count() const noexcept { return dispcnt_write_count_; }
    uint16_t last_dispcnt_written() const noexcept { return last_dispcnt_written_; }

    uint8_t Read8(uint32_t address) const override;
    uint16_t Read16(uint32_t address) const override;
    uint32_t Read32(uint32_t address) const override;
    void Write8(uint32_t address, uint8_t value) override;
    void Write16(uint32_t address, uint16_t value) override;
    void Write32(uint32_t address, uint32_t value) override;

private:
    void TryStartImmediateDma(uint32_t control_high_address);
    void ExecuteDmaTransfer(uint32_t channel, bool timed_trigger);
    uint16_t ReadIo16Direct(uint32_t address) const;
    uint32_t ReadIo32Direct(uint32_t address) const;
    void WriteIo16Direct(uint32_t address, uint16_t value);
    void WriteIo32Direct(uint32_t address, uint32_t value);

    static constexpr size_t kPaletteSize = 0x00000400u;
    static constexpr size_t kVramSize = 0x00018000u;
    static constexpr size_t kOamSize = 0x00000400u;
    static constexpr size_t kIoSize = 0x00000400u;

    std::vector<uint8_t> bios_;
    std::vector<uint8_t> ewram_;
    std::vector<uint8_t> iwram_;
    std::vector<uint8_t> rom_;
    std::array<uint8_t, kPaletteSize> palette_{};
    std::array<uint8_t, kVramSize> vram_{};
    std::array<uint8_t, kOamSize> oam_{};
    std::array<uint8_t, kIoSize> io_{};
    bool bios_loaded_ = false;
    uint16_t keyinput_state_ = 0x03FFu;
    uint8_t postflg_ = 0x01u;
    uint16_t waitcnt_ = 0x4317u;
    uint64_t dispcnt_write_count_ = 0;
    uint16_t last_dispcnt_written_ = 0;
};

} // namespace emugbds::ingba
