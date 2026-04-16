#pragma once

#include "emugbds/core/IEmuCore.h"

#include <memory>
#include <optional>
#include <string>
#include <cstdint>

namespace emugbds::core {

class ModeCoordinator {
public:
    ModeCoordinator(std::unique_ptr<IEmuCore> ds_core, std::unique_ptr<IEmuCore> gba_core);

    bool Boot(const BootRequest& request, std::string& error);
    bool RunFrame(std::string& error);

    EmuMode ActiveMode() const noexcept;
    IEmuCore* ActiveCore() const noexcept;
    const std::optional<TransitionEvent>& LastTransition() const noexcept;

private:
    static std::optional<EmuMode> ResolveMode(const BootRequest& request);
    bool SwitchToGbaFromSlot2(std::string& error);

    std::unique_ptr<IEmuCore> ds_core_;
    std::unique_ptr<IEmuCore> gba_core_;
    IEmuCore* active_core_ = nullptr;
    std::optional<std::filesystem::path> slot2_rom_path_;
    std::optional<TransitionEvent> last_transition_;
    bool trace_transition_ = false;
    uint64_t frame_index_ = 0;
};

} // namespace emugbds::core
