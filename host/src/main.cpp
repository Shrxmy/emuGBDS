#include "emugbds/boot/BootPolicy.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/cores/MGbaCoreAdapter.h"
#include "emugbds/cores/MelonDsCoreAdapter.h"
#include "emugbds/frontend/GbaSdlFrontend.h"
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
        MGba,
        InHouse,
    };

    emugbds::runtime::BootConfig boot{};
    uint64_t frames = 1;
    bool sdl_frontend = false;
    int scale = 3;
    uint64_t sdl_frames = 0;
    double speed = 1.0;
    GbaBackend gba_backend = GbaBackend::Auto;
};

void PrintUsage() {
    std::cout
        << "emuGBDS - unified DS/GBA host prototype\n"
        << "Usage:\n"
        << "  emugbds [--mode auto|ds|gba] [--gba-core auto|mgba|internal] [--nds <rom.nds>] [--gba <rom.gba>] [--slot2 <rom.gba>] [--bios-gba <bios.bin>] [--bios-nds7 <biosnds7.bin>] [--bios-nds9 <biosnds9.bin>] [--firmware-nds <firmware.bin>] [--boot-mode auto|ds-firmware|direct] [--trace-transition] [--frames N] [--sdl] [--scale N] [--sdl-frames N] [--speed X]\n"
        << "Controls (SDL frontend): X=A, Z=B, A=L, S=R, Enter=Start, Backspace=Select, Arrows=D-pad, Esc=Quit\n";
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
    if (text == "mgba") {
        out_backend = CliOptions::GbaBackend::MGba;
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
        if (arg == "--sdl") {
            options.sdl_frontend = true;
            continue;
        }
        if (arg == "--scale") {
            if (i + 1 >= argc) {
                error = "Missing value for --scale";
                return false;
            }
            options.scale = std::stoi(argv[++i]);
            continue;
        }
        if (arg == "--sdl-frames") {
            if (i + 1 >= argc) {
                error = "Missing value for --sdl-frames";
                return false;
            }
            options.sdl_frames = std::stoull(argv[++i]);
            continue;
        }
        if (arg == "--speed") {
            if (i + 1 >= argc) {
                error = "Missing value for --speed";
                return false;
            }
            options.speed = std::stod(argv[++i]);
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

#if EMUGBDS_HAS_MELONDS
    ds_core = std::make_unique<emugbds::cores::MelonDsCoreAdapter>();
#endif

    const bool wants_internal_gba = options.gba_backend == CliOptions::GbaBackend::InHouse;
    const bool wants_mgba_gba = options.gba_backend == CliOptions::GbaBackend::MGba;

#if EMUGBDS_HAS_MGBA
    if (!wants_internal_gba) {
        gba_core = std::make_unique<emugbds::cores::MGbaCoreAdapter>();
    }
#else
    if (wants_mgba_gba) {
        std::cerr << "Requested --gba-core mgba, but mGBA support is not compiled in.\n";
        return 1;
    }
#endif

    if (!gba_core) {
        gba_core = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();
    }

    emugbds::runtime::RuntimeKernel kernel(std::move(ds_core), std::move(gba_core));
    if (!kernel.Boot(options.boot, error)) {
        std::cerr << "Boot failed: " << error << '\n';
        return 1;
    }

    if (options.sdl_frontend) {
        if (dynamic_cast<emugbds::cores::InHouseGbaCoreAdapter*>(kernel.active_core()) != nullptr) {
            std::cerr << "SDL frontend currently requires --gba-core mgba; the in-house core is headless in this milestone.\n";
            return 1;
        }
        if (!emugbds::frontend::RunGbaSdlFrontend(kernel, options.scale, options.sdl_frames, options.speed, error)) {
            std::cerr << "SDL frontend failed: " << error << '\n';
            return 1;
        }
        return 0;
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
