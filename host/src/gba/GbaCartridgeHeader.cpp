#include "emugbds/gba/GbaCartridgeHeader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

namespace emugbds::gba {
namespace {

constexpr std::size_t kHeaderSize = 0xC0;
constexpr std::size_t kLogoOffset = 0x04;
constexpr std::size_t kLogoSize = 0x9C;
constexpr std::size_t kTitleOffset = 0xA0;
constexpr std::size_t kTitleSize = 12;
constexpr std::size_t kGameCodeOffset = 0xAC;
constexpr std::size_t kMakerCodeOffset = 0xB0;

constexpr std::array<uint8_t, kLogoSize> kNintendoLogo = {
    0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21, 0x3D, 0x84, 0x82, 0x0A, 0x84, 0xE4, 0x09, 0xAD,
    0x11, 0x24, 0x8B, 0x98, 0xC0, 0x81, 0x7F, 0x21, 0xA3, 0x52, 0xBE, 0x19, 0x93, 0x09, 0xCE, 0x20,
    0x10, 0x46, 0x4A, 0x4A, 0xF8, 0x27, 0x31, 0xEC, 0x58, 0xC7, 0xE8, 0x33, 0x82, 0xE3, 0xCE, 0xBF,
    0x85, 0xF4, 0xDF, 0x94, 0xCE, 0x4B, 0x09, 0xC1, 0x94, 0x56, 0x8A, 0xC0, 0x13, 0x72, 0xA7, 0xFC,
    0x9F, 0x84, 0x4D, 0x73, 0xA3, 0xCA, 0x9A, 0x61, 0x58, 0x97, 0xA3, 0x27, 0xFC, 0x03, 0x98, 0x76,
    0x23, 0x1D, 0xC7, 0x61, 0x03, 0x04, 0xAE, 0x56, 0xBF, 0x38, 0x84, 0x00, 0x40, 0xA7, 0x0E, 0xFD,
    0xFF, 0x52, 0xFE, 0x03, 0x6F, 0x95, 0x30, 0xF1, 0x97, 0xFB, 0xC0, 0x85, 0x60, 0xD6, 0x80, 0x25,
    0xA9, 0x63, 0xBE, 0x03, 0x01, 0x4E, 0x38, 0xE2, 0xF9, 0xA2, 0x34, 0xFF, 0xBB, 0x3E, 0x03, 0x44,
    0x78, 0x00, 0x90, 0xCB, 0x88, 0x11, 0x3A, 0x94, 0x65, 0xC0, 0x7C, 0x63, 0x87, 0xF0, 0x3C, 0xAF,
    0xD6, 0x25, 0xE4, 0x8B, 0x38, 0x0A, 0xAC, 0x72, 0x21, 0xD4, 0xF8, 0x07,
};

bool IsPrintableAscii(uint8_t value) {
    return value >= 0x20 && value <= 0x7E;
}

std::string DecodeAsciiField(const uint8_t* field, std::size_t size) {
    std::string decoded;
    decoded.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        if (field[i] == 0) {
            break;
        }
        if (!IsPrintableAscii(field[i])) {
            decoded.push_back('?');
            continue;
        }
        decoded.push_back(static_cast<char>(field[i]));
    }

    while (!decoded.empty() && decoded.back() == ' ') {
        decoded.pop_back();
    }
    return decoded;
}

} // namespace

bool InspectGbaCartridgeHeader(
    const std::filesystem::path& rom_path,
    GbaCartridgeHeaderInfo& info,
    std::string& error) {
    std::ifstream input(rom_path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open ROM for Slot-2 header inspection: " + rom_path.string();
        return false;
    }

    std::array<uint8_t, kHeaderSize> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()))) {
        error = "ROM is too small to contain a valid GBA cartridge header: " + rom_path.string();
        return false;
    }

    info.title = DecodeAsciiField(&header[kTitleOffset], kTitleSize);
    info.game_code = DecodeAsciiField(&header[kGameCodeOffset], 4);
    info.maker_code = DecodeAsciiField(&header[kMakerCodeOffset], 2);

    info.fixed_value = header[0xB2];
    info.unit_code = header[0xB3];
    info.device_type = header[0xB4];
    info.software_version = header[0xBC];
    info.complement = header[0xBD];

    info.fixed_byte_valid = info.fixed_value == 0x96;
    info.logo_valid = std::equal(
        kNintendoLogo.begin(),
        kNintendoLogo.end(),
        header.begin() + static_cast<std::ptrdiff_t>(kLogoOffset));

    unsigned sum = 0;
    for (std::size_t i = kTitleOffset; i <= 0xBC; ++i) {
        sum = (sum + header[i]) & 0xFFU;
    }
    info.computed_complement = static_cast<uint8_t>((0xE7U - sum) & 0xFFU);
    info.complement_valid = info.computed_complement == info.complement;

    return true;
}

} // namespace emugbds::gba
