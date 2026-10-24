#pragma once

#include "emugbds/ingba/CpuState.h"
#include "emugbds/ingba/MemoryBus.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace emugbds::ingba {

class Core {
public:
    struct UnsupportedArmStats {
        uint64_t generic = 0;
        uint64_t thumb = 0;
        uint64_t multiply = 0;
        uint64_t block_transfer = 0;
        uint64_t software_interrupt = 0;
    };

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
    uint16_t dispcnt() const noexcept;
    uint16_t dispstat() const noexcept;
    uint16_t vcount() const noexcept;
    uint16_t bgcnt(uint32_t index) const noexcept;
    uint16_t iflags() const noexcept;
    uint16_t ie() const noexcept;
    uint16_t ime() const noexcept;
    uint64_t dispcnt_write_count() const noexcept;
    uint16_t last_dispcnt_written() const noexcept;

    const CpuState& cpu() const noexcept { return cpu_; }
    uint64_t frame_counter() const noexcept { return frame_counter_; }
    uint64_t unknown_arm_instruction_count() const noexcept { return unknown_arm_instruction_count_; }
    const UnsupportedArmStats& unsupported_arm_stats() const noexcept { return unsupported_arm_stats_; }
    uint8_t last_unsupported_swi() const noexcept { return last_unsupported_swi_; }

private:
    enum class UnsupportedArmClass {
        Generic,
        Thumb,
        Multiply,
        BlockTransfer,
        SoftwareInterrupt,
    };

    struct AddResult {
        uint32_t value = 0;
        bool carry = false;
        bool overflow = false;
    };

    bool StepArm(std::string& error);
    bool ExecuteThumbInstruction(uint16_t instruction, uint32_t pc, uint32_t fetch_pc, std::string& error);
    bool ExecuteArmPsrTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmDataProcessing(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSingleDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmHalfwordTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmMultiply(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmMultiplyLong(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmBlockDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSoftwareInterrupt(uint32_t instruction, uint32_t next_pc, std::string& error);
    bool ExecuteSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error);
    bool HandleSoftwareInterrupt(uint8_t swi_imm, std::string& error);
    bool ExecuteCpuSet(bool fast);
    bool ExecuteIntrWait(bool force_vblank, std::string& error);
    bool ExecuteBgAffineSet();
    bool ExecuteLz77UnComp(bool vram_mode, std::string& error);
    bool ExecuteRlUnComp(bool vram_mode, std::string& error);
    bool HandleUnsupportedArm(UnsupportedArmClass unsupported_class, uint32_t next_pc, std::string& error);
    void RecordUnsupportedArm(UnsupportedArmClass unsupported_class);
    const char* DominantUnsupportedClassName() const noexcept;
    bool DecodeArmOperand2(uint32_t instruction, uint32_t fetch_pc, uint32_t& operand2, bool& shifter_carry) const;
    uint32_t DecodeShiftedRegister(uint32_t instruction, uint32_t fetch_pc, bool& shifter_carry) const;
    uint32_t ReadArmRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteArmRegister(uint32_t reg_index, uint32_t value);
    uint32_t ReadArmUserRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteArmUserRegister(uint32_t reg_index, uint32_t value);
    uint32_t ReadThumbRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteThumbRegister(uint32_t reg_index, uint32_t value);
    void SetThumbState(bool enabled) noexcept;
    void SetNFlag(bool value);
    void SetZFlag(bool value);
    void SetCFlag(bool value);
    void SetVFlag(bool value);
    void SetNZFlags(uint32_t value);
    bool ConditionPassed(uint32_t instruction) const;
    static uint32_t Ror(uint32_t value, uint32_t amount);
    static AddResult AddWithCarry(uint32_t lhs, uint32_t rhs, bool carry_in);
    static uint32_t SignExtend24(uint32_t value);
    static int32_t SignExtend(uint32_t value, uint32_t bits);
    static bool IsValidCpuMode(uint32_t mode) noexcept;
    static bool ModeHasSpsr(uint32_t mode) noexcept;
    uint32_t CurrentMode() const noexcept;
    bool CurrentModeIsPrivileged() const noexcept;
    void SaveBankedRegistersForMode(uint32_t mode) noexcept;
    void RestoreBankedRegistersForMode(uint32_t mode) noexcept;
    void SwitchCpuMode(uint32_t new_mode) noexcept;
    uint32_t ReadSpsrForMode(uint32_t mode) const noexcept;
    void WriteSpsrForMode(uint32_t mode, uint32_t value) noexcept;
    uint32_t ReadCurrentSpsr() const noexcept;
    bool WriteCurrentSpsr(uint32_t value) noexcept;
    void WriteCpsr(uint32_t value) noexcept;
    void EnterException(
        uint32_t target_mode,
        uint32_t handler_address,
        bool handler_thumb,
        uint32_t return_address,
        bool mask_irq) noexcept;
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
    };

    std::array<uint32_t, kVideoPixelCount> video_buffer_{};
    std::array<VideoScanlineState, kVideoHeight> video_scanline_states_{};
    MemoryBus bus_{};
    CpuState cpu_{};
    bool initialized_ = false;
    bool rom_loaded_ = false;
    bool thumb_bl_prefix_pending_ = false;
    bool halted_ = false;
    bool in_vblank_ = false;
    bool in_hblank_ = false;
    bool in_vcounter_match_ = false;
    uint32_t thumb_bl_prefix_base_ = 0;
    uint32_t lcd_cycle_in_frame_ = 0;
    uint64_t frame_counter_ = 0;
    uint64_t unknown_arm_instruction_count_ = 0;
    UnsupportedArmStats unsupported_arm_stats_{};
    uint8_t last_unsupported_swi_ = 0;
};

} // namespace emugbds::ingba
