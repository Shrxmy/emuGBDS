#include "emugbds/boot/BootPolicy.h"

#include <system_error>

namespace emugbds::boot {
namespace {

std::optional<std::filesystem::path> Find(const std::filesystem::path& relative,
                                          const std::vector<std::filesystem::path>& extra_roots) {
    std::error_code error;
    if (std::filesystem::exists(relative, error)) {
        return relative;
    }
    for (const std::filesystem::path& root : extra_roots) {
        const std::filesystem::path candidate = root / relative;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path> DetectDefaultGbaBios(const std::vector<std::filesystem::path>& extra_roots) {
    return Find(std::filesystem::path("bios") / "gb-bios" / "gba_bios.bin", extra_roots);
}

std::optional<std::filesystem::path> DetectDefaultNdsBios7(const std::vector<std::filesystem::path>& extra_roots) {
    return Find(std::filesystem::path("bios") / "nds-bios" / "biosnds7.bin", extra_roots);
}

std::optional<std::filesystem::path> DetectDefaultNdsBios9(const std::vector<std::filesystem::path>& extra_roots) {
    return Find(std::filesystem::path("bios") / "nds-bios" / "biosnds9.bin", extra_roots);
}

std::optional<std::filesystem::path> DetectDefaultNdsFirmware(const std::vector<std::filesystem::path>& extra_roots) {
    return Find(std::filesystem::path("bios") / "nds-bios" / "dsfirmware.bin", extra_roots);
}

void ResolveDefaultBootPaths(runtime::BootConfig& config, const std::vector<std::filesystem::path>& extra_roots) {
    if (!config.gba_bios_path.has_value()) {
        config.gba_bios_path = DetectDefaultGbaBios(extra_roots);
    }
    if (!config.nds_bios7_path.has_value()) {
        config.nds_bios7_path = DetectDefaultNdsBios7(extra_roots);
    }
    if (!config.nds_bios9_path.has_value()) {
        config.nds_bios9_path = DetectDefaultNdsBios9(extra_roots);
    }
    if (!config.nds_firmware_path.has_value()) {
        config.nds_firmware_path = DetectDefaultNdsFirmware(extra_roots);
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
