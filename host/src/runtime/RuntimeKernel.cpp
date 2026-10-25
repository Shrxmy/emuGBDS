#include "emugbds/runtime/RuntimeKernel.h"

#include "emugbds/cores/InHouseDsCoreAdapter.h"
#include "emugbds/cores/InHouseGbaCoreAdapter.h"

#include <iostream>

namespace emugbds::runtime {

RuntimeKernel::RuntimeKernel(std::unique_ptr<core::IEmuCore> ds_core, std::unique_ptr<core::IEmuCore> gba_core)
    : ds_core_(std::move(ds_core)),
      gba_core_(std::move(gba_core)) {}

bool RuntimeKernel::Boot(const BootConfig& config, std::string& error) {
    const std::optional<core::EmuMode> resolved_mode = ResolveMode(config);
    if (!resolved_mode.has_value()) {
        error = "Unable to resolve emulation mode from the provided arguments";
        state_ = RuntimeState::Error;
        return false;
    }

    state_ = RuntimeState::Booting;
    trace_transition_ = config.trace_transition;
    slot2_rom_path_ = config.slot2_rom_path;
    frame_index_ = 0;
    last_transition_.reset();
    last_output_ = {};

    core::BootRequest core_request = BuildCoreRequest(config);

    if (auto* inhouse_adapter = dynamic_cast<cores::InHouseGbaCoreAdapter*>(gba_core_.get())) {
        if (config.gba_bios_path.has_value() && !inhouse_adapter->SetBiosPath(*config.gba_bios_path, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
    } else if (config.gba_bios_path.has_value()) {
        error = "GBA BIOS path was provided but active GBA adapter does not support BIOS configuration";
        state_ = RuntimeState::Error;
        return false;
    }

    if (*resolved_mode == core::EmuMode::DS) {
        if (!ds_core_) {
            error = "DS mode requested but DS core is unavailable";
            state_ = RuntimeState::Error;
            return false;
        }
        if (!config.nds_rom_path.has_value() && !slot2_rom_path_.has_value()) {
            error = "DS mode requires --nds <path> or --slot2 <path> for firmware Slot-2 boot";
            state_ = RuntimeState::Error;
            return false;
        }
        if (!ds_core_->Configure(core_request, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        if (!ds_core_->Initialize(error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        if (slot2_rom_path_.has_value() && !ds_core_->SetSecondaryRom(*slot2_rom_path_, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        const std::filesystem::path primary_ds_path =
            config.nds_rom_path.has_value() ? *config.nds_rom_path : std::filesystem::path{};
        if (!ds_core_->LoadPrimaryRom(primary_ds_path, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        active_core_ = ds_core_.get();
    } else {
        if (!gba_core_) {
            error = "GBA mode requested but GBA core is unavailable";
            state_ = RuntimeState::Error;
            return false;
        }
        if (!gba_core_->Configure(core_request, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        const std::optional<std::filesystem::path> gba_path =
            config.gba_rom_path.has_value() ? config.gba_rom_path : config.slot2_rom_path;
        if (!gba_path.has_value()) {
            error = "GBA mode requires --gba <path> (or --slot2 <path> as fallback)";
            state_ = RuntimeState::Error;
            return false;
        }
        if (!gba_core_->Initialize(error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        if (!gba_core_->LoadPrimaryRom(*gba_path, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        active_core_ = gba_core_.get();
    }

    state_ = RuntimeState::Running;
    last_output_.active_mode = active_mode();
    last_output_.stats.state = state_;
    last_output_.stats.frame_index = frame_index_;
    return true;
}

bool RuntimeKernel::Tick(TickResult& result, std::string& error) {
    result = {};

    if (state_ == RuntimeState::Stopped) {
        result.status = TickStatus::Stopped;
        result.output = last_output_;
        return true;
    }

    if (state_ != RuntimeState::Running || !active_core_) {
        error = "Runtime kernel is not in a runnable state";
        state_ = RuntimeState::Error;
        return false;
    }

    if (!active_core_->RunFrame(error)) {
        state_ = RuntimeState::Error;
        return false;
    }

    ++frame_index_;
    last_output_.active_mode = active_mode();
    last_output_.stats.state = state_;
    last_output_.stats.frame_index = frame_index_;
    last_output_.transition.reset();
    last_output_.video.reset();
    last_output_.audio.reset();
    if (active_mode() == core::EmuMode::GBA) {
        if (const auto* inhouse_gba = dynamic_cast<const cores::InHouseGbaCoreAdapter*>(active_core_)) {
            VideoFrame frame{};
            if (inhouse_gba->GetVideoFrame(frame)) {
                last_output_.video = frame;
            }
            AudioChunk audio{};
            if (inhouse_gba->GetAudioChunk(audio)) {
                last_output_.audio = audio;
            }
        }
    } else if (const auto* inhouse_ds = dynamic_cast<const cores::InHouseDsCoreAdapter*>(active_core_)) {
        VideoFrame frame{};
        if (inhouse_ds->GetVideoFrame(frame)) {
            last_output_.video = frame;
        }
        AudioChunk audio{};
        if (inhouse_ds->GetAudioChunk(audio)) {
            last_output_.audio = audio;
        }
    }
    result.status = TickStatus::FrameAdvanced;

    if (trace_transition_ && active_mode() == core::EmuMode::DS) {
        std::cout << "[runtime] frame=" << frame_index_ << " mode=DS\n";
    }

    const std::optional<core::TransitionEvent> event = active_core_->PollTransition();
    if (!event.has_value()) {
        result.output = last_output_;
        return true;
    }

    last_transition_ = ConvertTransition(*event);
    last_output_.transition = last_transition_;
    state_ = RuntimeState::Transitioning;
    last_output_.stats.state = state_;

    std::cout << "[runtime] transition requested at frame " << frame_index_
              << ": " << last_transition_->reason << '\n';

    if (event->from_mode == core::EmuMode::DS && event->to_mode == core::EmuMode::GBA) {
        if (!SwitchToGbaFromSlot2(error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        // This frame's video and audio pointed into the DS core, which the switch just freed.
        last_output_.video.reset();
        last_output_.audio.reset();
        result.status = TickStatus::Transitioned;
    }

    state_ = RuntimeState::Running;
    last_output_.active_mode = active_mode();
    last_output_.stats.state = state_;
    result.output = last_output_;
    return true;
}

bool RuntimeKernel::InjectInput(const InputSnapshot& input, std::string& error) {
    if (state_ != RuntimeState::Running || !active_core_) {
        error = "Runtime kernel is not in a state that can accept input";
        return false;
    }
    if (active_mode() == core::EmuMode::GBA) {
        if (auto* inhouse_gba = dynamic_cast<cores::InHouseGbaCoreAdapter*>(active_core_)) {
            inhouse_gba->ApplyInputMasks(input.gba_press_mask, input.gba_release_mask);
        }
    } else if (auto* inhouse_ds = dynamic_cast<cores::InHouseDsCoreAdapter*>(active_core_)) {
        inhouse_ds->ApplyInput(input);
    }
    return true;
}

bool RuntimeKernel::PollOutputs(OutputBundle& output) const {
    output = last_output_;
    return true;
}

void RuntimeKernel::Shutdown() noexcept {
    if (ds_core_) {
        ds_core_->Shutdown();
    }
    if (gba_core_) {
        gba_core_->Shutdown();
    }
    active_core_ = nullptr;
    state_ = RuntimeState::Stopped;
    last_output_.stats.state = state_;
}

core::EmuMode RuntimeKernel::active_mode() const noexcept {
    if (!active_core_) {
        return core::EmuMode::DS;
    }
    return active_core_->mode();
}

core::IEmuCore* RuntimeKernel::active_core() const noexcept {
    return active_core_;
}

std::optional<core::EmuMode> RuntimeKernel::ResolveMode(const BootConfig& config) {
    switch (config.mode_hint) {
    case core::ModeHint::DS:
        return core::EmuMode::DS;
    case core::ModeHint::GBA:
        return core::EmuMode::GBA;
    case core::ModeHint::Auto:
        break;
    }

    if (config.nds_rom_path.has_value()) {
        return core::EmuMode::DS;
    }
    if (config.gba_rom_path.has_value() || config.slot2_rom_path.has_value()) {
        return core::EmuMode::GBA;
    }
    return std::nullopt;
}

core::BootRequest RuntimeKernel::BuildCoreRequest(const BootConfig& config) const {
    core::BootRequest request;
    request.mode_hint = config.mode_hint;
    request.nds_rom_path = config.nds_rom_path;
    request.gba_rom_path = config.gba_rom_path;
    request.slot2_rom_path = config.slot2_rom_path;
    request.nds_bios7_path = config.nds_bios7_path;
    request.nds_bios9_path = config.nds_bios9_path;
    request.nds_firmware_path = config.nds_firmware_path;
    request.ds_boot_mode = config.ds_boot_mode;
    request.ds_menu = config.ds_menu;
    request.trace_transition = config.trace_transition;
    return request;
}

bool RuntimeKernel::SwitchToGbaFromSlot2(std::string& error) {
    if (!gba_core_) {
        error = "Transition requested to GBA mode but GBA core is unavailable";
        return false;
    }
    if (!slot2_rom_path_.has_value()) {
        error = "Transition requested to GBA mode but no Slot-2 ROM is configured";
        return false;
    }
    if (!gba_core_->Initialize(error)) {
        return false;
    }
    if (!gba_core_->LoadPrimaryRom(*slot2_rom_path_, error)) {
        return false;
    }
    active_core_ = gba_core_.get();
    if (ds_core_) {
        ds_core_->Shutdown(); // the console is a GBA now: free the DS side (its saves were written at the handoff)
    }
    std::cout << "[runtime] switched active core to GBA using Slot-2 ROM '"
              << slot2_rom_path_->string() << "' at frame " << frame_index_ << ".\n";
    return true;
}

TransitionIntent RuntimeKernel::ConvertTransition(const core::TransitionEvent& event) {
    TransitionIntent intent;
    intent.from_mode = event.from_mode;
    intent.to_mode = event.to_mode;
    intent.reason = event.reason;
    intent.trigger = TransitionTrigger::CoreSignal;
    return intent;
}

} // namespace emugbds::runtime
