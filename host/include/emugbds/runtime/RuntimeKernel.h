#pragma once

#include "emugbds/core/IEmuCore.h"
#include "emugbds/runtime/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace emugbds::runtime {

class RuntimeKernel {
public:
    RuntimeKernel(std::unique_ptr<core::IEmuCore> ds_core, std::unique_ptr<core::IEmuCore> gba_core);

    bool Boot(const BootConfig& config, std::string& error);
    bool Tick(TickResult& result, std::string& error);
    bool InjectInput(const InputSnapshot& input, std::string& error);
    bool PollOutputs(OutputBundle& output) const;
    void Shutdown() noexcept;

    RuntimeState state() const noexcept { return state_; }
    core::EmuMode active_mode() const noexcept;
    core::IEmuCore* active_core() const noexcept;
    const std::optional<TransitionIntent>& last_transition() const noexcept { return last_transition_; }

private:
    static std::optional<core::EmuMode> ResolveMode(const BootConfig& config);
    core::BootRequest BuildCoreRequest(const BootConfig& config) const;
    bool SwitchToGbaFromSlot2(std::string& error);
    static TransitionIntent ConvertTransition(const core::TransitionEvent& event);

    std::unique_ptr<core::IEmuCore> ds_core_;
    std::unique_ptr<core::IEmuCore> gba_core_;
    core::IEmuCore* active_core_ = nullptr;

    RuntimeState state_ = RuntimeState::Uninitialized;
    std::optional<std::filesystem::path> slot2_rom_path_;
    std::optional<TransitionIntent> last_transition_;
    OutputBundle last_output_{};
    uint64_t frame_index_ = 0;
    bool trace_transition_ = false;
};

} // namespace emugbds::runtime
