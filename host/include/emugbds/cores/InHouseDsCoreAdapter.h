#pragma once

#include "emugbds/core/IEmuCore.h"
#include "emugbds/inds/Core.h"

#include <optional>

namespace emugbds::cores {

class InHouseDsCoreAdapter final : public core::IEmuCore {
public:
    core::EmuMode mode() const noexcept override;
    bool Configure(const core::BootRequest& request, std::string& error) override;
    bool Initialize(std::string& error) override;
    bool LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) override;
    bool RunFrame(std::string& error) override;
    bool Reset(std::string& error) override;
    bool IsRunning() const noexcept override;
    void Shutdown() noexcept override;
    bool SetSecondaryRom(const std::filesystem::path& rom_path, std::string& error) override;
    std::optional<core::TransitionEvent> PollTransition() override;

private:
    inds::Core core_{};
    bool configured_ = false;
    bool initialized_ = false;
    bool primary_loaded_ = false;
    std::optional<std::filesystem::path> nds_bios7_path_;
    std::optional<std::filesystem::path> nds_bios9_path_;
    std::optional<std::filesystem::path> nds_firmware_path_;
    std::optional<std::filesystem::path> slot2_rom_path_;
    core::DsBootMode ds_boot_mode_ = core::DsBootMode::Auto;
    bool trace_transition_ = false;
};

} // namespace emugbds::cores
