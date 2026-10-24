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

enum class DsBootMode {
    Auto,
    DsFirmware,
    Direct,
    // Run the real ARM9/ARM7 BIOS and firmware from reset (in bring-up).
    Bios,
};

struct BootRequest {
    ModeHint mode_hint = ModeHint::Auto;
    std::optional<std::filesystem::path> nds_rom_path;
    std::optional<std::filesystem::path> gba_rom_path;
    std::optional<std::filesystem::path> slot2_rom_path;
    std::optional<std::filesystem::path> nds_bios7_path;
    std::optional<std::filesystem::path> nds_bios9_path;
    std::optional<std::filesystem::path> nds_firmware_path;
    DsBootMode ds_boot_mode = DsBootMode::Auto;
    bool trace_transition = false;
};

struct TransitionEvent {
    EmuMode from_mode;
    EmuMode to_mode;
    std::string reason;
};

} // namespace emugbds::core
