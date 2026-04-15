#pragma once

#include "emugbds/core/Types.h"

#include <filesystem>
#include <optional>
#include <string>

namespace emugbds::core {

class IEmuCore {
public:
    virtual ~IEmuCore() = default;

    virtual EmuMode mode() const noexcept = 0;
    virtual bool Initialize(std::string& error) = 0;
    virtual bool LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) = 0;
    virtual bool RunFrame(std::string& error) = 0;
    virtual bool Reset(std::string& error) = 0;
    virtual bool IsRunning() const noexcept = 0;
    virtual void Shutdown() noexcept = 0;

    virtual bool SetSecondaryRom(const std::filesystem::path& rom_path, std::string& error) {
        (void)rom_path;
        error = "Secondary ROM is not supported by this core";
        return false;
    }

    virtual std::optional<TransitionEvent> PollTransition() {
        return std::nullopt;
    }
};

} // namespace emugbds::core
