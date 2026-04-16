#include "emugbds/boot/BootPolicy.h"
#include "emugbds/cores/InHouseDsCoreAdapter.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/runtime/RuntimeKernel.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

struct CliOptions {
    enum class GbaBackend {
        Auto,
        InHouse,
    };

    emugbds::runtime::BootConfig boot{};
    uint64_t frames = 1;
    GbaBackend gba_backend = GbaBackend::Auto;
};

void PrintUsage() {
    std::cout
        << "emuGBDS - unified DS/GBA host prototype\n"
        << "Usage:\n"
        << "  emugbds [--mode auto|ds|gba] [--gba-core auto|internal] [--nds <rom.nds>] [--gba <rom.gba>] [--slot2 <rom.gba>] [--bios-gba <bios.bin>] [--bios-nds7 <biosnds7.bin>] [--bios-nds9 <biosnds9.bin>] [--firmware-nds <firmware.bin>] [--boot-mode auto|ds-firmware|direct] [--trace-transition] [--frames N]\n";
}

bool ParseMode(const std::string& text, emugbds::core::ModeHint& out_mode) {
    if (text == "auto") {
        out_mode = emugbds::core::ModeHint::Auto;
        return true;
    }
    if (text == "ds") {
        out_mode = emugbds::core::ModeHint::DS;
        return true;
    }
    if (text == "gba") {
        out_mode = emugbds::core::ModeHint::GBA;
        return true;
    }
    return false;
}

bool ParseDsBootMode(const std::string& text, emugbds::core::DsBootMode& out_mode) {
    if (text == "auto") {
        out_mode = emugbds::core::DsBootMode::Auto;
        return true;
    }
    if (text == "ds-firmware") {
        out_mode = emugbds::core::DsBootMode::DsFirmware;
        return true;
    }
    if (text == "direct") {
        out_mode = emugbds::core::DsBootMode::Direct;
        return true;
    }
    return false;
}

bool ParseGbaBackend(const std::string& text, CliOptions::GbaBackend& out_backend) {
    if (text == "auto") {
        out_backend = CliOptions::GbaBackend::Auto;
        return true;
    }
    if (text == "internal") {
        out_backend = CliOptions::GbaBackend::InHouse;
        return true;
    }
    return false;
}

bool ParseArgs(int argc, char** argv, CliOptions& options, std::string& error) {
    bool positional_rom_consumed = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return false;
        }
        if (arg == "--mode") {
            if (i + 1 >= argc) {
                error = "Missing value for --mode";
                return false;
            }
            if (!ParseMode(argv[++i], options.boot.mode_hint)) {
                error = "Invalid --mode value";
                return false;
            }
            continue;
        }
        if (arg == "--gba-core") {
            if (i + 1 >= argc) {
                error = "Missing value for --gba-core";
                return false;
            }
            if (!ParseGbaBackend(argv[++i], options.gba_backend)) {
                error = "Invalid --gba-core value";
                return false;
            }
            continue;
        }
        if (arg == "--nds") {
            if (i + 1 >= argc) {
                error = "Missing value for --nds";
                return false;
            }
            options.boot.nds_rom_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--gba") {
            if (i + 1 >= argc) {
                error = "Missing value for --gba";
                return false;
            }
            options.boot.gba_rom_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--slot2") {
            if (i + 1 >= argc) {
                error = "Missing value for --slot2";
                return false;
            }
            options.boot.slot2_rom_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--bios-gba") {
            if (i + 1 >= argc) {
                error = "Missing value for --bios-gba";
                return false;
            }
            options.boot.gba_bios_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--bios-nds7") {
            if (i + 1 >= argc) {
                error = "Missing value for --bios-nds7";
                return false;
            }
            options.boot.nds_bios7_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--bios-nds9") {
            if (i + 1 >= argc) {
                error = "Missing value for --bios-nds9";
                return false;
            }
            options.boot.nds_bios9_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--firmware-nds") {
            if (i + 1 >= argc) {
                error = "Missing value for --firmware-nds";
                return false;
            }
            options.boot.nds_firmware_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--boot-mode") {
            if (i + 1 >= argc) {
                error = "Missing value for --boot-mode";
                return false;
            }
            if (!ParseDsBootMode(argv[++i], options.boot.ds_boot_mode)) {
                error = "Invalid --boot-mode value";
                return false;
            }
            continue;
        }
        if (arg == "--trace-transition") {
            options.boot.trace_transition = true;
            continue;
        }
        if (arg == "--frames") {
            if (i + 1 >= argc) {
                error = "Missing value for --frames";
                return false;
            }
            options.frames = std::stoull(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] != '-') {
            if (positional_rom_consumed) {
                error = "Multiple positional ROM paths were provided; use --gba/--nds/--slot2 for additional files";
                return false;
            }
            const std::filesystem::path rom_path(arg);
            const std::string ext = rom_path.extension().string();
            if (ext == ".gba" || ext == ".GBA") {
                options.boot.gba_rom_path = rom_path;
            } else if (ext == ".nds" || ext == ".NDS") {
                options.boot.nds_rom_path = rom_path;
            } else {
                error = "Unsupported positional ROM extension: " + ext;
                return false;
            }
            positional_rom_consumed = true;
            continue;
        }

        error = "Unknown argument: " + arg;
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
    CliOptions options;
    std::string error;
    if (!ParseArgs(argc, argv, options, error)) {
        if (!error.empty()) {
            std::cerr << "Argument error: " << error << '\n';
            PrintUsage();
            return 1;
        }
        return 0;
    }

    emugbds::boot::ResolveDefaultBootPaths(options.boot);

    std::unique_ptr<emugbds::core::IEmuCore> ds_core;
    std::unique_ptr<emugbds::core::IEmuCore> gba_core;

    ds_core = std::make_unique<emugbds::cores::InHouseDsCoreAdapter>();

    (void)options.gba_backend;
    gba_core = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();

    emugbds::runtime::RuntimeKernel kernel(std::move(ds_core), std::move(gba_core));
    if (!kernel.Boot(options.boot, error)) {
        std::cerr << "Boot failed: " << error << '\n';
        return 1;
    }

    emugbds::runtime::TickResult tick_result{};
    for (uint64_t frame = 0; frame < options.frames; ++frame) {
        if (!kernel.Tick(tick_result, error)) {
            std::cerr << "Execution stopped on frame " << frame << ": " << error << '\n';
            return 1;
        }
        if (tick_result.status == emugbds::runtime::TickStatus::Stopped) {
            break;
        }
    }

    const auto& transition = kernel.last_transition();
    if (transition.has_value()) {
        std::cout << "Transition: " << transition->reason << '\n';
    }

    std::cout << "Completed " << options.frames << " frame(s) in mode "
              << (kernel.active_mode() == emugbds::core::EmuMode::DS ? "DS" : "GBA") << ".\n";
    return 0;
}
