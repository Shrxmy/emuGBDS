#pragma once

#include "emugbds/core/IEmuCore.h"
#include "emugbds/ingba/Core.h"

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

private:
    ingba::Core core_{};
    bool initialized_ = false;
    bool loaded_ = false;
    std::optional<std::filesystem::path> bios_path_;
};

} // namespace emugbds::cores
