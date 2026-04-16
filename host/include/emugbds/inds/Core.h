#pragma once

#include "emugbds/core/Types.h"
#include "emugbds/gba/GbaCartridgeHeader.h"
#include "emugbds/inds/CpuState.h"
#include "emugbds/inds/MemoryBus.h"
#include "emugbds/nds/NdsCartridgeHeader.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace emugbds::inds {

class Core {
public:
    bool Configure(
        const std::optional<std::filesystem::path>& nds_bios7_path,
        const std::optional<std::filesystem::path>& nds_bios9_path,
        const std::optional<std::filesystem::path>& nds_firmware_path,
        core::DsBootMode boot_mode,
        bool trace_transition,
        std::string& error);
    bool Initialize(std::string& error);
    bool LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error);
    bool LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error);
    bool RunFrame(std::string& error);
    bool Reset(std::string& error);
    bool IsRunning() const noexcept { return initialized_ && running_; }
    std::optional<core::TransitionEvent> PollTransition();

    const CpuState& arm9() const noexcept { return arm9_; }
    const CpuState& arm7() const noexcept { return arm7_; }
    uint64_t frame_counter() const noexcept { return frame_counter_; }
    std::optional<nds::NdsCartridgeHeaderInfo> nds_header() const noexcept { return nds_header_; }
    std::optional<gba::GbaCartridgeHeaderInfo> slot2_header() const noexcept { return slot2_header_; }

private:
    static uint32_t AdvancePc(uint32_t pc, uint32_t instructions, bool thumb);
    bool ShouldRequestFirmwareSlot2Transition() const noexcept;

    MemoryBus bus_{};
    CpuState arm9_{};
    CpuState arm7_{};
    std::optional<nds::NdsCartridgeHeaderInfo> nds_header_;
    std::optional<gba::GbaCartridgeHeaderInfo> slot2_header_;
    std::optional<core::TransitionEvent> pending_transition_;

    bool initialized_ = false;
    bool configured_ = false;
    bool running_ = false;
    bool trace_transition_ = false;
    bool nds_rom_loaded_ = false;
    bool slot2_rom_loaded_ = false;
    core::DsBootMode boot_mode_ = core::DsBootMode::Auto;
    uint64_t frame_counter_ = 0;
    uint64_t firmware_menu_frame_counter_ = 0;
};

} // namespace emugbds::inds
