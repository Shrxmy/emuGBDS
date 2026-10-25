#include "emugbds/frontend/Settings.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <system_error>

namespace emugbds::frontend {
namespace {

void ReadInt(const std::string& text, int min, int max, int& out) {
    int value = 0;
    const auto [end, result] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result == std::errc{} && end == text.data() + text.size() && value >= min && value <= max) {
        out = value;
    }
}

} // namespace

void Settings::AddRecentRom(const std::string& path) {
    recent_roms.erase(std::remove(recent_roms.begin(), recent_roms.end(), path), recent_roms.end());
    recent_roms.insert(recent_roms.begin(), path);
    if (recent_roms.size() > kMaxRecentRoms) {
        recent_roms.resize(kMaxRecentRoms);
    }
}


bool Settings::Load(const std::filesystem::path& path, std::string& error) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return true;
    }
    std::ifstream input(path);
    if (!input.is_open()) {
        error = "Failed to open settings file: " + path.u8string();
        return false;
    }
    std::vector<std::string> recent;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1u);
        if (key == "gba_bios") {
            gba_bios = value;
        } else if (key == "nds_bios7") {
            nds_bios7 = value;
        } else if (key == "nds_bios9") {
            nds_bios9 = value;
        } else if (key == "nds_firmware") {
            nds_firmware = value;
        } else if (key == "last_directory") {
            last_directory = value;
        } else if (key == "recent") {
            if (!value.empty() && recent.size() < kMaxRecentRoms) {
                recent.push_back(value);
            }
        } else if (key == "gba_scale") {
            ReadInt(value, 1, kMaxScale, gba_scale);
        } else if (key == "ds_scale") {
            ReadInt(value, 1, kMaxScale, ds_scale);
        } else if (key == "ds_layout") {
            if (value == "vertical") {
                ds_layout = DsLayout::Vertical;
            } else if (value == "horizontal") {
                ds_layout = DsLayout::Horizontal;
            }
        } else if (key == "fast_forward_speed") {
            ReadInt(value, 0, 16, fast_forward_speed);
        } else if (key == "gba_via_ds_firmware") {
            gba_via_ds_firmware = value == "1";
        } else if (key == "volume") {
            ReadInt(value, 0, 100, volume);
        } else if (key == "muted") {
            muted = value == "1";
        } else if (key == "pause_on_focus_loss") {
            pause_on_focus_loss = value == "1";
        } else if (key.rfind("key.", 0) == 0 && !value.empty()) {
            for (size_t i = 0; i < kButtonCount; ++i) {
                if (key.compare(4, std::string::npos, kButtonNames[i]) == 0) {
                    keys[i] = value;
                }
            }
        } else if (key.rfind("pad.", 0) == 0 && !value.empty()) {
            for (size_t i = 0; i < kButtonCount; ++i) {
                if (key.compare(4, std::string::npos, kButtonNames[i]) == 0) {
                    pad[i] = value;
                }
            }
        }
    }
    if (input.bad()) {
        error = "Failed to read settings file: " + path.u8string();
        return false;
    }
    if (!recent.empty()) {
        recent_roms = recent;
    }
    return true;
}


bool Settings::Save(const std::filesystem::path& path, std::string& error) const {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output.is_open()) {
            error = "Failed to write settings file: " + temporary.u8string();
            return false;
        }
        output << "# emuGBDS settings\n"
               << "gba_bios=" << gba_bios << "\n"
               << "nds_bios7=" << nds_bios7 << "\n"
               << "nds_bios9=" << nds_bios9 << "\n"
               << "nds_firmware=" << nds_firmware << "\n"
               << "last_directory=" << last_directory << "\n";
        for (const std::string& rom : recent_roms) {
            output << "recent=" << rom << "\n";
        }
        output << "gba_scale=" << gba_scale << "\n"
               << "ds_scale=" << ds_scale << "\n"
               << "ds_layout=" << (ds_layout == DsLayout::Horizontal ? "horizontal" : "vertical") << "\n"
               << "fast_forward_speed=" << fast_forward_speed << "\n"
               << "gba_via_ds_firmware=" << (gba_via_ds_firmware ? 1 : 0) << "\n"
               << "volume=" << volume << "\n"
               << "muted=" << (muted ? 1 : 0) << "\n"
               << "pause_on_focus_loss=" << (pause_on_focus_loss ? 1 : 0) << "\n";
        for (size_t i = 0; i < kButtonCount; ++i) {
            output << "key." << kButtonNames[i] << "=" << keys[i] << "\n";
        }
        for (size_t i = 0; i < kButtonCount; ++i) {
            output << "pad." << kButtonNames[i] << "=" << pad[i] << "\n";
        }
        output.flush();
        if (output.fail()) {
            error = "Failed to write settings file: " + temporary.u8string();
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        error = "Failed to replace settings file: " + path.u8string() + " (" + rename_error.message() + ")";
        return false;
    }
    return true;
}

} // namespace emugbds::frontend
