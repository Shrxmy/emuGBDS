#pragma once

#include "emugbds/core/IEmuCore.h"

#include <optional>

struct mCore;

namespace emugbds::cores {

class MGbaCoreAdapter final : public core::IEmuCore {
public:
    MGbaCoreAdapter() = default;
    ~MGbaCoreAdapter() override;

    core::EmuMode mode() const noexcept override;
    bool Initialize(std::string& error) override;
    bool LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) override;
    bool RunFrame(std::string& error) override;
    bool Reset(std::string& error) override;
    bool IsRunning() const noexcept override;
    void Shutdown() noexcept override;
    bool SetBiosPath(const std::filesystem::path& bios_path, std::string& error);

#if EMUGBDS_HAS_MGBA
    mCore* NativeCore() const noexcept { return core_; }
#else
    void* NativeCore() const noexcept { return nullptr; }
#endif

private:
    mCore* core_ = nullptr;
    bool loaded_ = false;
    std::optional<std::filesystem::path> bios_path_;
};

} // namespace emugbds::cores
