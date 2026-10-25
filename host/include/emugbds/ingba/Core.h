#pragma once

#include "emugbds/arm/ArmCpu.h"
#include "emugbds/ingba/MemoryBus.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::ingba {

class Core final : public arm::ICpuHost {
public:
    using UnsupportedArmStats = arm::ArmCpu::UnsupportedArmStats;

    Core() = default;
    // cpu_ is bound to this object's bus_ and host; a copy or move would stay bound to the source.
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    bool Initialize(std::string& error);
    bool LoadBios(const std::filesystem::path& bios_path, std::string& error);
    bool LoadRom(const std::filesystem::path& rom_path, std::string& error);
    bool RunFrame(std::string& error);
    bool Reset(std::string& error);
    // HALT stops the CPU, not the emulation core; timing and video continue.
    bool IsRunning() const noexcept { return initialized_ && rom_loaded_; }
    void ApplyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept;
    uint16_t keyinput_state() const noexcept { return bus_.keyinput_state(); }
    const uint32_t* video_pixels() const noexcept { return video_buffer_.data(); }
    uint32_t video_width() const noexcept { return kVideoWidth; }
    uint32_t video_height() const noexcept { return kVideoHeight; }
    // Interleaved stereo samples at Apu::kSampleRate produced by the last RunFrame.
    const std::vector<int16_t>& audio_samples() const noexcept { return bus_.apu().samples(); }
    uint16_t dispcnt() const noexcept;
    uint16_t dispstat() const noexcept;
    uint16_t vcount() const noexcept;
    uint16_t bgcnt(uint32_t index) const noexcept;
    uint16_t iflags() const noexcept;
    uint16_t ie() const noexcept;
    uint16_t ime() const noexcept;
    uint64_t dispcnt_write_count() const noexcept;
    uint16_t last_dispcnt_written() const noexcept;

    const arm::CpuState& cpu() const noexcept { return cpu_.state(); }
    const MemoryBus& bus() const noexcept { return bus_; }
    // Debug: layers to leave out (bits 0-3 BG0-BG3, bit 4 OBJ).
    void set_hidden_layers(uint8_t mask) noexcept { hidden_layers_ = mask; }
    uint64_t frame_counter() const noexcept { return frame_counter_; }
    uint64_t unknown_arm_instruction_count() const noexcept { return cpu_.unknown_arm_instruction_count(); }
    const UnsupportedArmStats& unsupported_arm_stats() const noexcept { return cpu_.unsupported_arm_stats(); }
    uint8_t last_unsupported_swi() const noexcept { return last_unsupported_swi_; }

    // arm::ICpuHost: enters the BIOS SWI vector, or emulates the call when no BIOS is loaded.
    bool OnSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) override;

private:
    bool HandleSoftwareInterrupt(uint8_t swi_imm, std::string& error);
    bool ExecuteCpuSet(bool fast);
    bool ExecuteIntrWait(bool force_vblank, std::string& error);
    bool ExecuteBgAffineSet();
    bool ExecuteLz77UnComp(bool vram_mode, std::string& error);
    bool ExecuteRlUnComp(bool vram_mode, std::string& error);
    void ServicePendingInterrupts() noexcept;
    void AdvanceLcdTiming(uint32_t elapsed_cycles) noexcept;
    void CaptureVideoScanlineState(uint16_t scanline) noexcept;
    void UpdateVideoFrame();

    static constexpr uint32_t kVideoWidth = 240u;
    static constexpr uint32_t kVideoHeight = 160u;
    static constexpr size_t kVideoPixelCount = static_cast<size_t>(kVideoWidth) * static_cast<size_t>(kVideoHeight);

    struct VideoScanlineState {
        uint16_t dispcnt = 0;
        std::array<uint16_t, 4u> bgcnt{};
        std::array<uint16_t, 4u> hofs{};
        std::array<uint16_t, 4u> vofs{};
        std::array<int16_t, 4u> affine_pa{};
        std::array<int16_t, 4u> affine_pb{};
        std::array<int16_t, 4u> affine_pc{};
        std::array<int16_t, 4u> affine_pd{};
        std::array<int32_t, 4u> reference_x{};
        std::array<int32_t, 4u> reference_y{};
        uint16_t blend_control = 0;
        uint16_t blend_alpha = 0;
        uint16_t blend_y = 0;
    };

    std::array<uint32_t, kVideoPixelCount> video_buffer_{};
    std::array<VideoScanlineState, kVideoHeight> video_scanline_states_{};
    MemoryBus bus_{};
    arm::ArmCpu cpu_{bus_, *this, arm::ArmArchitecture::V4T}; // Declared after bus_, which it references.
    bool initialized_ = false;
    bool rom_loaded_ = false;
    bool halted_ = false;
    bool in_vblank_ = false;
    bool in_hblank_ = false;
    bool in_vcounter_match_ = false;
    uint32_t lcd_cycle_in_frame_ = 0;
    uint64_t frame_counter_ = 0;
    uint8_t last_unsupported_swi_ = 0;
    uint8_t hidden_layers_ = 0;
};

} // namespace emugbds::ingba
