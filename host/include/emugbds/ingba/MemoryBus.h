#pragma once

#include "emugbds/ingba/Bus.h"

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

    uint8_t Read8(uint32_t address) const override;
    uint16_t Read16(uint32_t address) const override;
    uint32_t Read32(uint32_t address) const override;
    void Write8(uint32_t address, uint8_t value) override;
    void Write16(uint32_t address, uint16_t value) override;
    void Write32(uint32_t address, uint32_t value) override;

private:
    std::vector<uint8_t> bios_;
    std::vector<uint8_t> ewram_;
    std::vector<uint8_t> iwram_;
    std::vector<uint8_t> rom_;
    bool bios_loaded_ = false;
};

} // namespace emugbds::ingba
