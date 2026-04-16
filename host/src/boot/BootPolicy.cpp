#include "emugbds/boot/BootPolicy.h"

namespace emugbds::boot {

std::optional<std::filesystem::path> DetectDefaultGbaBios() {
    const std::filesystem::path default_path = std::filesystem::path("bios") / "gb-bios" / "gba_bios.bin";
    if (!std::filesystem::exists(default_path)) {
        return std::nullopt;
    }
    return default_path;
}

std::optional<std::filesystem::path> DetectDefaultNdsBios7() {
    const std::filesystem::path default_path = std::filesystem::path("bios") / "nds-bios" / "biosnds7.bin";
    if (!std::filesystem::exists(default_path)) {
        return std::nullopt;
    }
    return default_path;
}

std::optional<std::filesystem::path> DetectDefaultNdsBios9() {
    const std::filesystem::path default_path = std::filesystem::path("bios") / "nds-bios" / "biosnds9.bin";
    if (!std::filesystem::exists(default_path)) {
        return std::nullopt;
    }
    return default_path;
}

std::optional<std::filesystem::path> DetectDefaultNdsFirmware() {
    const std::filesystem::path default_path = std::filesystem::path("bios") / "nds-bios" / "dsfirmware.bin";
    if (!std::filesystem::exists(default_path)) {
        return std::nullopt;
    }
    return default_path;
}

void ResolveDefaultBootPaths(runtime::BootConfig& config) {
    if (!config.gba_bios_path.has_value()) {
        config.gba_bios_path = DetectDefaultGbaBios();
    }
    if (!config.nds_bios7_path.has_value()) {
        config.nds_bios7_path = DetectDefaultNdsBios7();
    }
    if (!config.nds_bios9_path.has_value()) {
        config.nds_bios9_path = DetectDefaultNdsBios9();
    }
    if (!config.nds_firmware_path.has_value()) {
        config.nds_firmware_path = DetectDefaultNdsFirmware();
    }

    if (config.ds_boot_mode != core::DsBootMode::Auto) {
        return;
    }

    if (config.nds_bios7_path.has_value() &&
        config.nds_bios9_path.has_value() &&
        config.nds_firmware_path.has_value()) {
        config.ds_boot_mode = core::DsBootMode::DsFirmware;
    } else {
        config.ds_boot_mode = core::DsBootMode::Direct;
    }
}

} // namespace emugbds::boot
