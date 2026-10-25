#pragma once

// Settings for the interactive app, stored as `key=value` lines (emugbds.ini
// next to the executable). Paths are UTF-8. Unknown keys and malformed values
// are ignored so an old or hand-edited file still loads.

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::frontend {

enum class DsLayout { Vertical, Horizontal };

struct Settings {
    static constexpr size_t kButtonCount = 12u;
    static constexpr size_t kMaxRecentRoms = 8u;
    static constexpr int kMaxScale = 6;
    // Game buttons in input::GbaKey bit order.
    static constexpr std::array<const char*, kButtonCount> kButtonNames = {
        "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y"};

    // Empty paths fall back to the files under bios/ in the working directory.
    std::string gba_bios;
    std::string nds_bios7;
    std::string nds_bios9;
    std::string nds_firmware;
    std::string last_directory;
    std::vector<std::string> recent_roms; // most recent first
    int gba_scale = 3;
    int ds_scale = 2;
    DsLayout ds_layout = DsLayout::Vertical;
    int fast_forward_speed = 4; // emulated frames per shown frame; 0 = unlimited
    bool gba_via_ds_firmware = false;
    // SDL scancode names, one per game button.
    std::array<std::string, kButtonCount> keys = {
        "Z", "X", "Right Shift", "Return", "Right", "Left", "Up", "Down", "S", "A", "D", "C"};

    void AddRecentRom(const std::string& path);
    // A missing file leaves the defaults and succeeds.
    bool Load(const std::filesystem::path& path, std::string& error);
    // Writes a temporary file and renames it over `path`.
    bool Save(const std::filesystem::path& path, std::string& error) const;
};

} // namespace emugbds::frontend
