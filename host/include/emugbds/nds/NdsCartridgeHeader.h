#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace emugbds::nds {

struct NdsCartridgeHeaderInfo {
    std::string title;
    std::string game_code;
    std::string maker_code;

    uint8_t unit_code = 0;
    uint8_t device_type = 0;
    uint8_t device_capacity = 0;
    uint8_t rom_version = 0;
    uint8_t autostart = 0;

    uint32_t arm9_rom_offset = 0;
    uint32_t arm9_entry_address = 0;
    uint32_t arm9_ram_address = 0;
    uint32_t arm9_size = 0;

    uint32_t arm7_rom_offset = 0;
    uint32_t arm7_entry_address = 0;
    uint32_t arm7_ram_address = 0;
    uint32_t arm7_size = 0;

    bool has_dual_arm_payload = false;
};

bool InspectNdsCartridgeHeader(
    const std::filesystem::path& rom_path,
    NdsCartridgeHeaderInfo& info,
    std::string& error);

} // namespace emugbds::nds
