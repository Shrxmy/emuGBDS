#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace emugbds::core {

enum class EmuMode {
    DS,
    GBA,
};

enum class ModeHint {
    Auto,
    DS,
    GBA,
};

struct BootRequest {
    ModeHint mode_hint = ModeHint::Auto;
    std::optional<std::filesystem::path> nds_rom_path;
    std::optional<std::filesystem::path> gba_rom_path;
    std::optional<std::filesystem::path> slot2_rom_path;
};

struct TransitionEvent {
    EmuMode from_mode;
    EmuMode to_mode;
    std::string reason;
};

} // namespace emugbds::core
