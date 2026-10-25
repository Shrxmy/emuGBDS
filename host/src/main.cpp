#include "emugbds/boot/BootPolicy.h"
#include "emugbds/core/HostPerformance.h"
#include "emugbds/frontend/Session.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if EMUGBDS_HAS_SDL2
#include "frontend/App.h"
#endif

namespace {

struct CliOptions {
    enum class GbaBackend {
        Auto,
        InHouse,
    };

    emugbds::runtime::BootConfig boot{};
    uint64_t frames = 1;
    bool frames_explicit = false;
    GbaBackend gba_backend = GbaBackend::Auto;
    bool interactive = false;
    bool uncapped = false;
    bool debug_stats = false;
    bool no_gba_bios = false;
};

void PrintUsage() {
    std::cout
        << "emuGBDS " EMUGBDS_VERSION " - unified DS/GBA host prototype\n"
        << "Usage:\n"
        << "  emugbds [--mode auto|ds|gba] [--gba-core auto|internal] [--nds <rom.nds>] [--gba <rom.gba>] [--slot2 <rom.gba>] [--bios-gba <bios.bin>] [--no-bios-gba] [--bios-nds7 <biosnds7.bin>] [--bios-nds9 <biosnds9.bin>] [--firmware-nds <firmware.bin>] [--boot-mode auto|ds-firmware|direct] [--ds-menu] [--trace-transition] [--frames N] [--interactive] [--uncapped] [--debug-stats] [--version] [rom.gba|rom.nds]\n"
        << "Without --frames the window opens (with --frames N it runs N frames headless, unless --interactive).\n";
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
        if (arg == "--version") {
            std::cout << "emuGBDS " EMUGBDS_VERSION "\n";
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
        if (arg == "--no-bios-gba") {
            options.no_gba_bios = true;
            options.boot.gba_bios_path.reset();
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
        if (arg == "--ds-menu") {
            // Boot the DS menu with the --nds card in Slot-1 (and --slot2 in Slot-2) instead of starting it.
            options.boot.ds_menu = true;
            options.boot.ds_boot_mode = emugbds::core::DsBootMode::DsFirmware;
            if (options.boot.mode_hint == emugbds::core::ModeHint::Auto) {
                options.boot.mode_hint = emugbds::core::ModeHint::DS;
            }
            continue;
        }
        if (arg == "--trace-transition") {
            options.boot.trace_transition = true;
            continue;
        }
        if (arg == "--interactive") {
            options.interactive = true;
            continue;
        }
        if (arg == "--uncapped") {
            options.uncapped = true;
            continue;
        }
        if (arg == "--debug-stats") {
            options.debug_stats = true;
            continue;
        }
        if (arg == "--frames") {
            if (i + 1 >= argc) {
                error = "Missing value for --frames";
                return false;
            }
            options.frames = std::stoull(argv[++i]);
            options.frames_explicit = true;
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

// Per-CPU state plus the most-hit unemulated I/O ports and memory blocks: what the DS game needs next.
void PrintDsBringUpReport(const emugbds::inds::Core& core) {
    const auto print_top = [](const char* kind, const std::map<uint32_t, uint64_t>& counts) {
        std::vector<std::pair<uint64_t, uint32_t>> ranked;
        for (const auto& [address, count] : counts) {
            ranked.emplace_back(count, address);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        std::cout << "  unmapped " << kind << " (" << counts.size() << " addresses):";
        for (size_t i = 0; i < std::min<size_t>(10u, ranked.size()); ++i) {
            std::cout << " 0x" << std::hex << std::setw(8) << std::setfill('0') << ranked[i].second
                      << std::dec << " x" << ranked[i].first;
        }
        std::cout << "\n";
    };
    const auto print_cpu = [&](const char* name, const emugbds::arm::ArmCpu& cpu, const emugbds::inds::AccessLog& log, int index) {
        const auto& state = cpu.state();
        const auto& stats = cpu.unsupported_arm_stats();
        std::cout << "DS " << name << ": PC=0x" << std::hex << std::setw(8) << std::setfill('0') << state.r[15]
                  << " CPSR=0x" << std::setw(8) << state.cpsr << std::dec
                  << " unsupported total=" << cpu.unknown_arm_instruction_count()
                  << " (generic=" << stats.generic << " thumb=" << stats.thumb << " multiply=" << stats.multiply
                  << " block=" << stats.block_transfer << " swi=" << stats.software_interrupt << ")\n";
        print_top("reads", log.reads);
        print_top("writes", log.writes);
        const auto& io = core.memory().io();
        std::cout << "  IME=" << io.ime(index) << " IE=0x" << std::hex << std::setw(8) << io.ie(index)
                  << " IF=0x" << std::setw(8) << io.iflags(index) << std::dec << "\n";
    };
    print_cpu("ARM9", core.arm9_cpu(), core.memory().arm9_unmapped(), 0);
    std::cout << "  ARM9 halted (CP15 wait for interrupt): " << (core.arm9_halted() ? "yes" : "no") << "\n";
    print_cpu("ARM7", core.arm7_cpu(), core.memory().arm7_unmapped(), 1);
    std::cout << "  ARM7 halted (HALTCNT): " << (core.arm7_halted() ? "yes" : "no") << "\n";
}

} // namespace

int main(int argc, char** argv) {
    emugbds::core::PreferPerformanceCores(); // emulation runs on this thread
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

    const bool has_rom = options.boot.nds_rom_path.has_value() || options.boot.gba_rom_path.has_value() ||
        options.boot.slot2_rom_path.has_value();
#if EMUGBDS_HAS_SDL2
    if (!options.frames_explicit) {
        options.interactive = true; // double-clicked or "Open with": headless runs always give --frames
    }
#endif

    std::unique_ptr<emugbds::frontend::Session> session;
    uint64_t executed_frames = 0;
    if (options.interactive) {
#if EMUGBDS_HAS_SDL2
        emugbds::frontend::AppOptions app;
        app.boot = options.boot;
        app.has_rom = has_rom;
        app.no_gba_bios = options.no_gba_bios;
        app.frame_limit = options.frames_explicit ? options.frames : 0u;
        app.uncapped = options.uncapped;
        if (!emugbds::frontend::RunApp(app, session, executed_frames, error)) {
            std::cerr << "Interactive execution failed: " << error << '\n';
            return 1;
        }
        if (!session) {
            return 0; // closed without a game running
        }
#else
        std::cerr << "Interactive execution failed: Interactive mode requested but SDL2 support is not enabled in this build\n";
        return 1;
#endif
    } else {
        emugbds::boot::ResolveDefaultBootPaths(options.boot);
        if (options.no_gba_bios) {
            options.boot.gba_bios_path.reset();
        }
        session = emugbds::frontend::Session::Start(options.boot, error);
        if (!session) {
            std::cerr << "Boot failed: " << error << '\n';
            return 1;
        }
        emugbds::runtime::TickResult tick_result{};
        for (uint64_t frame = 0; frame < options.frames; ++frame) {
            if (!session->kernel().Tick(tick_result, error)) {
                std::cerr << "Execution stopped on frame " << frame << ": " << error << '\n';
                return 1;
            }
            ++executed_frames;
            if (tick_result.status == emugbds::runtime::TickStatus::Stopped) {
                break;
            }
        }
    }

    emugbds::runtime::RuntimeKernel& kernel = session->kernel();
    const emugbds::cores::InHouseGbaCoreAdapter* gba_inhouse_raw = &session->gba();
    const auto& transition = kernel.last_transition();
    if (transition.has_value()) {
        std::cout << "Transition: " << transition->reason << '\n';
    }

    std::cout << "Completed " << executed_frames << " frame(s) in mode "
              << (kernel.active_mode() == emugbds::core::EmuMode::DS ? "DS" : "GBA") << ".\n";
    if (options.debug_stats && kernel.active_mode() == emugbds::core::EmuMode::DS) {
        PrintDsBringUpReport(session->ds().core());
    }
    if (options.debug_stats && kernel.active_mode() == emugbds::core::EmuMode::GBA) {
        const auto& stats = gba_inhouse_raw->unsupported_arm_stats();
        std::cout << "GBA unsupported stats: total=" << gba_inhouse_raw->unknown_arm_instruction_count()
                  << " generic=" << stats.generic
                  << " thumb=" << stats.thumb
                  << " multiply=" << stats.multiply
                  << " block_transfer=" << stats.block_transfer
                  << " swi=" << stats.software_interrupt
                  << " last_swi=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(gba_inhouse_raw->last_unsupported_swi()) << std::dec << "\n";
        std::cout << std::hex << std::setfill('0')
                  << "GBA regs: DISPCNT=0x" << std::setw(4) << gba_inhouse_raw->dispcnt()
                  << " DISPSTAT=0x" << std::setw(4) << gba_inhouse_raw->dispstat()
                  << " VCOUNT=0x" << std::setw(4) << gba_inhouse_raw->vcount()
                  << " BG0CNT=0x" << std::setw(4) << gba_inhouse_raw->bgcnt(0)
                  << " BG1CNT=0x" << std::setw(4) << gba_inhouse_raw->bgcnt(1)
                  << " BG2CNT=0x" << std::setw(4) << gba_inhouse_raw->bgcnt(2)
                  << " BG3CNT=0x" << std::setw(4) << gba_inhouse_raw->bgcnt(3)
                  << " IE=0x" << std::setw(4) << gba_inhouse_raw->ie()
                  << " IME=0x" << std::setw(4) << gba_inhouse_raw->ime()
                  << " IF=0x" << std::setw(4) << gba_inhouse_raw->iflags()
                  << " PC=0x" << std::setw(8) << gba_inhouse_raw->pc()
                  << " SP=0x" << std::setw(8) << gba_inhouse_raw->sp()
                  << " LR=0x" << std::setw(8) << gba_inhouse_raw->lr()
                  << " CPSR=0x" << std::setw(8) << gba_inhouse_raw->cpsr()
                  << " DISPCNT_WRITES=" << std::dec << gba_inhouse_raw->dispcnt_write_count()
                  << std::hex
                  << " LAST_DISPCNT=0x" << std::setw(4) << gba_inhouse_raw->last_dispcnt_written()
                  << std::dec << "\n";
    }
    return 0;
}
