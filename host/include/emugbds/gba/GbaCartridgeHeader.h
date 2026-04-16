#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace emugbds::gba {

struct GbaCartridgeHeaderInfo {
    std::string title;
    std::string game_code;
    std::string maker_code;

    uint8_t fixed_value = 0;
    uint8_t unit_code = 0;
    uint8_t device_type = 0;
    uint8_t software_version = 0;
    uint8_t complement = 0;
    uint8_t computed_complement = 0;

    bool fixed_byte_valid = false;
    bool logo_valid = false;
    bool complement_valid = false;
};

bool InspectGbaCartridgeHeader(
    const std::filesystem::path& rom_path,
    GbaCartridgeHeaderInfo& info,
    std::string& error);

} // namespace emugbds::gba
