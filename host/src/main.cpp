#include "emugbds/core/ModeCoordinator.h"
#include "emugbds/cores/MGbaCoreAdapter.h"
#include "emugbds/cores/MelonDsCoreAdapter.h"
#include "emugbds/frontend/GbaSdlFrontend.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct CliOptions {
    emugbds::core::ModeHint mode_hint = emugbds::core::ModeHint::Auto;
    std::optional<std::filesystem::path> nds_rom_path;
    std::optional<std::filesystem::path> gba_rom_path;
    std::optional<std::filesystem::path> slot2_rom_path;
    std::optional<std::filesystem::path> gba_bios_path;
    uint64_t frames = 1;
    bool sdl_frontend = false;
    int scale = 3;
    uint64_t sdl_frames = 0;
    double speed = 1.0;
};

void PrintUsage() {
    std::cout
        << "emuGBDS - unified DS/GBA host prototype\n"
        << "Usage:\n"
        << "  emugbds [--mode auto|ds|gba] [--nds <rom.nds>] [--gba <rom.gba>] [--slot2 <rom.gba>] [--bios-gba <bios.bin>] [--frames N] [--sdl] [--scale N] [--sdl-frames N] [--speed X]\n"
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
            if (!ParseMode(argv[++i], options.mode_hint)) {
                error = "Invalid --mode value";
                return false;
            }
            continue;
        }
        if (arg == "--nds") {
            if (i + 1 >= argc) {
                error = "Missing value for --nds";
                return false;
            }
            options.nds_rom_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--gba") {
            if (i + 1 >= argc) {
                error = "Missing value for --gba";
                return false;
            }
            options.gba_rom_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--slot2") {
            if (i + 1 >= argc) {
                error = "Missing value for --slot2";
                return false;
            }
            options.slot2_rom_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--bios-gba") {
            if (i + 1 >= argc) {
                error = "Missing value for --bios-gba";
                return false;
            }
            options.gba_bios_path = std::filesystem::path(argv[++i]);
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
                options.gba_rom_path = rom_path;
            } else if (ext == ".nds" || ext == ".NDS") {
                options.nds_rom_path = rom_path;
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

std::optional<std::filesystem::path> DetectDefaultGbaBios() {
    const std::filesystem::path default_path = std::filesystem::path("bios") / "gb-bios" / "gba_bios.bin";
    if (!std::filesystem::exists(default_path)) {
        return std::nullopt;
    }
    return default_path;
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

    emugbds::core::BootRequest request;
    request.mode_hint = options.mode_hint;
    request.nds_rom_path = options.nds_rom_path;
    request.gba_rom_path = options.gba_rom_path;
    request.slot2_rom_path = options.slot2_rom_path;

    std::unique_ptr<emugbds::core::IEmuCore> ds_core;
    std::unique_ptr<emugbds::core::IEmuCore> gba_core;
    emugbds::cores::MGbaCoreAdapter* gba_adapter_for_setup = nullptr;

#if EMUGBDS_HAS_MELONDS
    ds_core = std::make_unique<emugbds::cores::MelonDsCoreAdapter>();
#endif
#if EMUGBDS_HAS_MGBA
    gba_core = std::make_unique<emugbds::cores::MGbaCoreAdapter>();
    gba_adapter_for_setup = dynamic_cast<emugbds::cores::MGbaCoreAdapter*>(gba_core.get());
#endif

    if (!options.gba_bios_path.has_value()) {
        options.gba_bios_path = DetectDefaultGbaBios();
    }

    if (options.gba_bios_path.has_value()) {
        if (!gba_adapter_for_setup) {
            std::cerr << "GBA BIOS was provided but mGBA adapter is unavailable in this build.\n";
            return 1;
        }
        if (!gba_adapter_for_setup->SetBiosPath(*options.gba_bios_path, error)) {
            std::cerr << "BIOS setup failed: " << error << '\n';
            return 1;
        }
    }

    emugbds::core::ModeCoordinator coordinator(std::move(ds_core), std::move(gba_core));
    if (!coordinator.Boot(request, error)) {
        std::cerr << "Boot failed: " << error << '\n';
        return 1;
    }

    if (options.sdl_frontend) {
        auto* active = coordinator.ActiveCore();
        if (!active || active->mode() != emugbds::core::EmuMode::GBA) {
            std::cerr << "SDL frontend is currently supported only for active GBA mode.\n";
            return 1;
        }

        auto* gba_adapter = dynamic_cast<emugbds::cores::MGbaCoreAdapter*>(active);
        if (!gba_adapter) {
            std::cerr << "Active GBA core is not the expected adapter type.\n";
            return 1;
        }

        if (!emugbds::frontend::RunGbaSdlFrontend(*gba_adapter, options.scale, options.sdl_frames, options.speed, error)) {
            std::cerr << "SDL frontend failed: " << error << '\n';
            return 1;
        }

        return 0;
    }

    for (uint64_t frame = 0; frame < options.frames; ++frame) {
        if (!coordinator.RunFrame(error)) {
            std::cerr << "Execution stopped on frame " << frame << ": " << error << '\n';
            return 1;
        }
    }

    const auto transition = coordinator.LastTransition();
    if (transition.has_value()) {
        std::cout << "Transition: " << transition->reason << '\n';
    }

    std::cout << "Completed " << options.frames << " frame(s) in mode "
              << (coordinator.ActiveMode() == emugbds::core::EmuMode::DS ? "DS" : "GBA") << ".\n";
    return 0;
}
