#pragma once

#include "emugbds/arm/ArmCpu.h"
#include "emugbds/core/Types.h"
#include "emugbds/gba/GbaCartridgeHeader.h"
#include "emugbds/inds/MemoryBus.h"
#include "emugbds/nds/NdsCartridgeHeader.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace emugbds::inds {

class Core {
public:
    Core() = default;
    // The CPUs are bound to this object's buses and hosts.
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

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

    const arm::CpuState& arm9() const noexcept { return arm9_.state(); }
    const arm::CpuState& arm7() const noexcept { return arm7_.state(); }
    const arm::ArmCpu& arm9_cpu() const noexcept { return arm9_; }
    const arm::ArmCpu& arm7_cpu() const noexcept { return arm7_; }
    const MemoryBus& memory() const noexcept { return bus_; }
    bool arm9_halted() const noexcept { return arm9_halted_; }
    uint64_t frame_counter() const noexcept { return frame_counter_; }
    std::optional<nds::NdsCartridgeHeaderInfo> nds_header() const noexcept { return nds_header_; }
    std::optional<gba::GbaCartridgeHeaderInfo> slot2_header() const noexcept { return slot2_header_; }

private:
    // Each CPU's system hooks: BIOS SWI vectors, and CP15 on the ARM9.
    class CpuHost final : public arm::ICpuHost {
    public:
        CpuHost(Core& core, bool arm9) noexcept : core_(core), arm9_(arm9) {}
        bool OnSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) override;
        bool OnCoprocessorRead(uint32_t coprocessor, uint32_t opcode1, uint32_t crn, uint32_t crm,
                               uint32_t opcode2, uint32_t& value) override;
        bool OnCoprocessorWrite(uint32_t coprocessor, uint32_t opcode1, uint32_t crn, uint32_t crm,
                                uint32_t opcode2, uint32_t value) override;

    private:
        Core& core_;
        bool arm9_;
    };

    bool DirectBoot(std::string& error);
    bool ShouldRequestFirmwareSlot2Transition() const noexcept;

    MemoryBus bus_{};
    CpuHost arm9_host_{*this, true};
    CpuHost arm7_host_{*this, false};
    arm::ArmCpu arm9_{bus_.arm9(), arm9_host_, arm::ArmArchitecture::V5TE};
    arm::ArmCpu arm7_{bus_.arm7(), arm7_host_, arm::ArmArchitecture::V4T};
    std::optional<nds::NdsCartridgeHeaderInfo> nds_header_;
    std::optional<gba::GbaCartridgeHeaderInfo> slot2_header_;
    std::optional<core::TransitionEvent> pending_transition_;

    bool initialized_ = false;
    bool configured_ = false;
    bool running_ = false;
    bool trace_transition_ = false;
    bool nds_rom_loaded_ = false;
    bool slot2_rom_loaded_ = false;
    bool cpus_active_ = false; // Only a direct-booted cartridge runs code; the firmware menu is not emulated.
    bool arm9_halted_ = false;
    core::DsBootMode boot_mode_ = core::DsBootMode::Auto;
    uint64_t frame_counter_ = 0;
    uint64_t firmware_menu_frame_counter_ = 0;
};

} // namespace emugbds::inds
