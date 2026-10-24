#include "emugbds/boot/BootPolicy.h"
#include "emugbds/cores/InHouseDsCoreAdapter.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/input/GbaKeys.h"
#include "emugbds/runtime/RuntimeKernel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if EMUGBDS_HAS_SDL2
#define SDL_MAIN_HANDLED
#include <SDL.h>
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
        << "emuGBDS - unified DS/GBA host prototype\n"
        << "Usage:\n"
        << "  emugbds [--mode auto|ds|gba] [--gba-core auto|internal] [--nds <rom.nds>] [--gba <rom.gba>] [--slot2 <rom.gba>] [--bios-gba <bios.bin>] [--no-bios-gba] [--bios-nds7 <biosnds7.bin>] [--bios-nds9 <biosnds9.bin>] [--firmware-nds <firmware.bin>] [--boot-mode auto|ds-firmware|direct|bios] [--trace-transition] [--frames N] [--interactive] [--uncapped] [--debug-stats]\n";
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
    if (text == "bios") {
        out_mode = emugbds::core::DsBootMode::Bios;
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

#if EMUGBDS_HAS_SDL2
uint32_t KeyMaskFromScancode(SDL_Scancode scancode) {
    using emugbds::input::GbaKey;
    using emugbds::input::GbaKeyMask;
    switch (scancode) {
    case SDL_SCANCODE_Z:
        return GbaKeyMask(GbaKey::A);
    case SDL_SCANCODE_X:
        return GbaKeyMask(GbaKey::B);
    case SDL_SCANCODE_RETURN:
        return GbaKeyMask(GbaKey::Start);
    case SDL_SCANCODE_RSHIFT:
        return GbaKeyMask(GbaKey::Select);
    case SDL_SCANCODE_RIGHT:
        return GbaKeyMask(GbaKey::Right);
    case SDL_SCANCODE_LEFT:
        return GbaKeyMask(GbaKey::Left);
    case SDL_SCANCODE_UP:
        return GbaKeyMask(GbaKey::Up);
    case SDL_SCANCODE_DOWN:
        return GbaKeyMask(GbaKey::Down);
    case SDL_SCANCODE_A:
        return GbaKeyMask(GbaKey::L);
    case SDL_SCANCODE_S:
        return GbaKeyMask(GbaKey::R);
    default:
        return 0u;
    }
}
#endif

bool RunInteractive(
    emugbds::runtime::RuntimeKernel& kernel,
    const CliOptions& options,
    uint64_t& executed_frames,
    std::string& error) {
#if !EMUGBDS_HAS_SDL2
    (void)kernel;
    (void)options;
    (void)executed_frames;
    error = "Interactive mode requested but SDL2 support is not enabled in this build";
    return false;
#else
    executed_frames = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }

    SDL_Window* window = SDL_CreateWindow(
        "emuGBDS (GBA Interactive)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        240 * 3,
        160 * 3,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_Quit();
        return false;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        240,
        160);
    if (!texture) {
        error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    // Audio is optional: without a device the loop falls back to the frame timer.
    SDL_AudioDeviceID audio_device = 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{};
        want.freq = 32768;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0); // SDL converts to the device format.
        if (audio_device != 0) {
            SDL_PauseAudioDevice(audio_device, 0);
        }
    }
    if (audio_device == 0) {
        std::cerr << "[audio] disabled: " << SDL_GetError() << "\n";
    }
    // About 60 ms of queued stereo int16 audio: enough to ride out frame jitter.
    constexpr uint32_t kAudioQueueTargetBytes = 2048u * 2u * sizeof(int16_t);

    bool running = true;
    uint32_t held_mask = 0u;
    uint32_t previous_mask = 0u;
    const bool frame_limit_enabled = options.frames_explicit;
    const auto frame_target = std::chrono::duration<double>(1.0 / 59.7275);

    while (running && (!frame_limit_enabled || executed_frames < options.frames)) {
        const auto frame_begin = std::chrono::steady_clock::now();

        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                    break;
                }
                if (event.key.repeat == 0) {
                    held_mask |= KeyMaskFromScancode(event.key.keysym.scancode);
                }
            } else if (event.type == SDL_KEYUP) {
                if (event.key.repeat == 0) {
                    held_mask &= ~KeyMaskFromScancode(event.key.keysym.scancode);
                }
            }
        }
        if (!running) {
            break;
        }

        emugbds::runtime::InputSnapshot input{};
        input.gba_press_mask = held_mask & ~previous_mask;
        input.gba_release_mask = previous_mask & ~held_mask;
        previous_mask = held_mask;
        if (input.gba_press_mask != 0u || input.gba_release_mask != 0u) {
            if (!kernel.InjectInput(input, error)) {
                SDL_DestroyTexture(texture);
                SDL_DestroyRenderer(renderer);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return false;
            }
        }

        emugbds::runtime::TickResult tick_result{};
        if (!kernel.Tick(tick_result, error)) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return false;
        }
        if (tick_result.status == emugbds::runtime::TickStatus::Stopped) {
            break;
        }
        ++executed_frames;

        emugbds::runtime::OutputBundle output{};
        kernel.PollOutputs(output);
        if (output.video.has_value() && output.video->pixels != nullptr) {
            const auto& frame = *output.video;
            SDL_UpdateTexture(texture, nullptr, frame.pixels, static_cast<int>(frame.width * sizeof(uint32_t)));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        if (audio_device != 0 && output.audio.has_value()) {
            if (SDL_GetQueuedAudioSize(audio_device) > kAudioQueueTargetBytes * 4u) {
                // Uncapped runs or a stalled device outpace playback; drop instead of lagging.
                SDL_ClearQueuedAudio(audio_device);
            }
            SDL_QueueAudio(audio_device, output.audio->samples,
                static_cast<uint32_t>(output.audio->sample_count * sizeof(int16_t)));
        }

        if (!options.uncapped) {
            if (audio_device != 0 && output.audio.has_value()) {
                // The GBA emits 32768 samples per emulated second, so draining to the target paces real time.
                // The deadline keeps a stalled device from freezing input and video.
                const auto deadline = frame_begin + frame_target * 2;
                while (SDL_GetQueuedAudioSize(audio_device) > kAudioQueueTargetBytes &&
                       std::chrono::steady_clock::now() < deadline) {
                    SDL_Delay(1);
                }
            } else {
                const auto elapsed = std::chrono::steady_clock::now() - frame_begin;
                if (elapsed < frame_target) {
                    std::this_thread::sleep_for(frame_target - elapsed);
                }
            }
        }
    }

    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return true;
#endif
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
    if (options.no_gba_bios) {
        options.boot.gba_bios_path.reset();
    }

    std::unique_ptr<emugbds::core::IEmuCore> ds_core;
    std::unique_ptr<emugbds::core::IEmuCore> gba_core;
    emugbds::cores::InHouseGbaCoreAdapter* gba_inhouse_raw = nullptr;

    auto inhouse_ds = std::make_unique<emugbds::cores::InHouseDsCoreAdapter>();
    const emugbds::cores::InHouseDsCoreAdapter* ds_inhouse_raw = inhouse_ds.get();
    ds_core = std::move(inhouse_ds);

    (void)options.gba_backend;
    auto inhouse_gba = std::make_unique<emugbds::cores::InHouseGbaCoreAdapter>();
    gba_inhouse_raw = inhouse_gba.get();
    gba_core = std::move(inhouse_gba);

    emugbds::runtime::RuntimeKernel kernel(std::move(ds_core), std::move(gba_core));
    if (!kernel.Boot(options.boot, error)) {
        std::cerr << "Boot failed: " << error << '\n';
        return 1;
    }

    uint64_t executed_frames = 0;
    if (options.interactive) {
        if (!RunInteractive(kernel, options, executed_frames, error)) {
            std::cerr << "Interactive execution failed: " << error << '\n';
            return 1;
        }
    } else {
        emugbds::runtime::TickResult tick_result{};
        for (uint64_t frame = 0; frame < options.frames; ++frame) {
            if (!kernel.Tick(tick_result, error)) {
                std::cerr << "Execution stopped on frame " << frame << ": " << error << '\n';
                return 1;
            }
            ++executed_frames;
            if (tick_result.status == emugbds::runtime::TickStatus::Stopped) {
                break;
            }
        }
    }

    const auto& transition = kernel.last_transition();
    if (transition.has_value()) {
        std::cout << "Transition: " << transition->reason << '\n';
    }

    std::cout << "Completed " << executed_frames << " frame(s) in mode "
              << (kernel.active_mode() == emugbds::core::EmuMode::DS ? "DS" : "GBA") << ".\n";
    if (options.debug_stats && kernel.active_mode() == emugbds::core::EmuMode::DS) {
        PrintDsBringUpReport(ds_inhouse_raw->core());
    }
    if (options.debug_stats && gba_inhouse_raw != nullptr && kernel.active_mode() == emugbds::core::EmuMode::GBA) {
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
