#pragma once

#include "emugbds/core/IEmuCore.h"
#include "emugbds/ingba/Core.h"
#include "emugbds/runtime/Types.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace emugbds::cores {

class InHouseGbaCoreAdapter final : public core::IEmuCore {
public:
    core::EmuMode mode() const noexcept override;
    bool Initialize(std::string& error) override;
    bool LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) override;
    bool RunFrame(std::string& error) override;
    bool Reset(std::string& error) override;
    bool IsRunning() const noexcept override;
    void Shutdown() noexcept override;

    bool SetBiosPath(const std::filesystem::path& bios_path, std::string& error);
    void ApplyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept;
    bool GetVideoFrame(runtime::VideoFrame& out_frame) const noexcept;
    bool GetAudioChunk(runtime::AudioChunk& out_chunk) const noexcept;
    uint16_t keyinput_state() const noexcept { return core_->keyinput_state(); }
    uint64_t unknown_arm_instruction_count() const noexcept { return core_->unknown_arm_instruction_count(); }
    const ingba::Core::UnsupportedArmStats& unsupported_arm_stats() const noexcept { return core_->unsupported_arm_stats(); }
    uint8_t last_unsupported_swi() const noexcept { return core_->last_unsupported_swi(); }
    uint16_t dispcnt() const noexcept { return core_->dispcnt(); }
    uint16_t dispstat() const noexcept { return core_->dispstat(); }
    uint16_t vcount() const noexcept { return core_->vcount(); }
    uint16_t bgcnt(uint32_t index) const noexcept { return core_->bgcnt(index); }
    uint16_t iflags() const noexcept { return core_->iflags(); }
    uint16_t ie() const noexcept { return core_->ie(); }
    uint16_t ime() const noexcept { return core_->ime(); }
    uint32_t pc() const noexcept { return core_->cpu().r[15]; }
    uint32_t sp() const noexcept { return core_->cpu().r[13]; }
    uint32_t lr() const noexcept { return core_->cpu().r[14]; }
    uint32_t cpsr() const noexcept { return core_->cpu().cpsr; }
    uint64_t dispcnt_write_count() const noexcept { return core_->dispcnt_write_count(); }
    uint16_t last_dispcnt_written() const noexcept { return core_->last_dispcnt_written(); }
    const ingba::Core& core() const noexcept { return *core_; }
    void SetHiddenLayers(uint8_t mask) noexcept {
        if (core_) {
            core_->set_hidden_layers(mask);
        }
    }

private:
    // Heap-owned: the core is large and holds a CPU bound to its own bus, so Initialize replaces it instead of assigning.
    // Allocated by Initialize and freed by Shutdown, so an unused GBA core costs nothing.
    std::unique_ptr<ingba::Core> core_;
    bool initialized_ = false;
    bool loaded_ = false;
    std::optional<std::filesystem::path> bios_path_;
};

} // namespace emugbds::cores
