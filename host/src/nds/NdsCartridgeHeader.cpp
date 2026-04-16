#include "emugbds/nds/NdsCartridgeHeader.h"

#include <array>
#include <cctype>
#include <fstream>

namespace emugbds::nds {
namespace {

constexpr std::size_t kHeaderReadSize = 0x200;

uint32_t ReadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

bool IsPrintableAscii(uint8_t value) {
    return value >= 0x20 && value <= 0x7E;
}

std::string DecodeAsciiField(const uint8_t* field, std::size_t size) {
    std::string decoded;
    decoded.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        const uint8_t value = field[i];
        if (value == 0) {
            break;
        }
        decoded.push_back(IsPrintableAscii(value) ? static_cast<char>(value) : '?');
    }
    while (!decoded.empty() && decoded.back() == ' ') {
        decoded.pop_back();
    }
    return decoded;
}

} // namespace

bool InspectNdsCartridgeHeader(
    const std::filesystem::path& rom_path,
    NdsCartridgeHeaderInfo& info,
    std::string& error) {
    std::ifstream input(rom_path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open NDS ROM for header inspection: " + rom_path.string();
        return false;
    }

    std::array<uint8_t, kHeaderReadSize> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()))) {
        error = "NDS ROM is too small to contain a valid header: " + rom_path.string();
        return false;
    }

    info.title = DecodeAsciiField(&header[0x00], 12);
    info.game_code = DecodeAsciiField(&header[0x0C], 4);
    info.maker_code = DecodeAsciiField(&header[0x10], 2);
    info.unit_code = header[0x12];
    info.device_type = header[0x13];
    info.device_capacity = header[0x14];
    info.rom_version = header[0x1E];
    info.autostart = header[0x1F];

    info.arm9_rom_offset = ReadLe32(&header[0x20]);
    info.arm9_entry_address = ReadLe32(&header[0x24]);
    info.arm9_ram_address = ReadLe32(&header[0x28]);
    info.arm9_size = ReadLe32(&header[0x2C]);

    info.arm7_rom_offset = ReadLe32(&header[0x30]);
    info.arm7_entry_address = ReadLe32(&header[0x34]);
    info.arm7_ram_address = ReadLe32(&header[0x38]);
    info.arm7_size = ReadLe32(&header[0x3C]);

    info.has_dual_arm_payload =
        info.arm9_size > 0 && info.arm7_size > 0 &&
        info.arm9_rom_offset > 0 && info.arm7_rom_offset > 0;
    return true;
}

} // namespace emugbds::nds
