#pragma once

#include "emugbds/inds/Bus.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::inds {

class MemoryBus final : public IBus {
public:
    bool LoadBios7(const std::filesystem::path& bios_path, std::string& error);
    bool LoadBios9(const std::filesystem::path& bios_path, std::string& error);
    bool LoadFirmware(const std::filesystem::path& firmware_path, std::string& error);
    bool LoadNdsRom(const std::filesystem::path& rom_path, std::string& error);
    bool LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error);
    void ClearNdsRom();
    void ClearSlot2Rom();

    bool HasBios7() const noexcept { return !bios7_.empty(); }
    bool HasBios9() const noexcept { return !bios9_.empty(); }
    bool HasFirmware() const noexcept { return !firmware_.empty(); }
    bool HasNdsRom() const noexcept { return !nds_rom_.empty(); }
    bool HasSlot2Rom() const noexcept { return !slot2_rom_.empty(); }

    uint8_t Read8(uint32_t address) const override;
    uint16_t Read16(uint32_t address) const override;
    uint32_t Read32(uint32_t address) const override;
    void Write8(uint32_t address, uint8_t value) override;
    void Write16(uint32_t address, uint16_t value) override;
    void Write32(uint32_t address, uint32_t value) override;

private:
    std::vector<uint8_t> bios7_;
    std::vector<uint8_t> bios9_;
    std::vector<uint8_t> firmware_;
    std::vector<uint8_t> main_ram_;
    std::vector<uint8_t> shared_wram_;
    std::vector<uint8_t> nds_rom_;
    std::vector<uint8_t> slot2_rom_;
};

} // namespace emugbds::inds
