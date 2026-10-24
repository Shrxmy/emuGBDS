#pragma once

#include "emugbds/core/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace emugbds::runtime {

enum class RuntimeState {
    Uninitialized,
    Booting,
    Running,
    Transitioning,
    Stopped,
    Error,
};

enum class TransitionTrigger {
    CoreSignal,
    UserRequest,
    Unknown,
};

struct BootConfig {
    core::ModeHint mode_hint = core::ModeHint::Auto;
    std::optional<std::filesystem::path> nds_rom_path;
    std::optional<std::filesystem::path> gba_rom_path;
    std::optional<std::filesystem::path> slot2_rom_path;
    std::optional<std::filesystem::path> gba_bios_path;
    std::optional<std::filesystem::path> nds_bios7_path;
    std::optional<std::filesystem::path> nds_bios9_path;
    std::optional<std::filesystem::path> nds_firmware_path;
    core::DsBootMode ds_boot_mode = core::DsBootMode::Auto;
    bool trace_transition = false;
};

struct TransitionIntent {
    core::EmuMode from_mode = core::EmuMode::DS;
    core::EmuMode to_mode = core::EmuMode::GBA;
    TransitionTrigger trigger = TransitionTrigger::Unknown;
    std::string reason;
};

struct RuntimeStats {
    RuntimeState state = RuntimeState::Uninitialized;
    uint64_t frame_index = 0;
};

enum class PixelFormat {
    ABGR8888,
};

struct VideoFrame {
    PixelFormat format = PixelFormat::ABGR8888;
    uint32_t width = 0;
    uint32_t height = 0;
    const uint32_t* pixels = nullptr;
};

struct AudioChunk {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    const int16_t* samples = nullptr;
    size_t sample_count = 0; // Total int16 values; channels are interleaved.
};

struct OutputBundle {
    core::EmuMode active_mode = core::EmuMode::DS;
    RuntimeStats stats{};
    std::optional<TransitionIntent> transition;
    std::optional<VideoFrame> video;
    std::optional<AudioChunk> audio;
};

enum class TickStatus {
    FrameAdvanced,
    Transitioned,
    NoFrame,
    Stopped,
};

struct TickResult {
    TickStatus status = TickStatus::NoFrame;
    OutputBundle output{};
};

struct InputSnapshot {
    // Bit masks use emugbds::input::GbaKey values.
    uint32_t gba_press_mask = 0;
    uint32_t gba_release_mask = 0;
    // DS touchscreen: the pen position on the bottom screen in pixels.
    bool touch_down = false;
    uint32_t touch_x = 0;
    uint32_t touch_y = 0;
};

} // namespace emugbds::runtime
