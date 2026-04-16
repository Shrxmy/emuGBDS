#include "emugbds/runtime/RuntimeKernel.h"

#include "emugbds/cores/InHouseGbaCoreAdapter.h"
#include "emugbds/cores/MGbaCoreAdapter.h"

#if EMUGBDS_HAS_MGBA
#include <mgba/core/core.h>
#endif

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
    gba_av_ready_ = false;
    gba_av_supported_ = false;
    gba_video_pipeline_.Reset();
    gba_audio_pipeline_.Reset();
    ClearAvOutputs();

    core::BootRequest core_request = BuildCoreRequest(config);

    if (auto* gba_adapter = dynamic_cast<cores::MGbaCoreAdapter*>(gba_core_.get())) {
        if (config.gba_bios_path.has_value() && !gba_adapter->SetBiosPath(*config.gba_bios_path, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
    } else if (auto* inhouse_adapter = dynamic_cast<cores::InHouseGbaCoreAdapter*>(gba_core_.get())) {
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
        if (!config.nds_rom_path.has_value()) {
            error = "DS mode requires --nds <path>";
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
        if (!ds_core_->LoadPrimaryRom(*config.nds_rom_path, error)) {
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
        gba_av_supported_ = dynamic_cast<cores::MGbaCoreAdapter*>(active_core_) != nullptr;
        if (gba_av_supported_ && !SetupGbaAvPipelines(error)) {
            state_ = RuntimeState::Error;
            return false;
        }
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
    ClearAvOutputs();
    result.status = TickStatus::FrameAdvanced;

    if (active_mode() == core::EmuMode::GBA && gba_av_supported_) {
        if (!gba_av_ready_ && !SetupGbaAvPipelines(error)) {
            state_ = RuntimeState::Error;
            return false;
        }

        runtime::VideoFrame video_frame{};
        if (!gba_video_pipeline_.CaptureGbaFrame(video_frame, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        last_output_.video = video_frame;

        runtime::AudioChunk audio_chunk{};
        if (!gba_audio_pipeline_.PullGbaAudio(audio_chunk, error)) {
            state_ = RuntimeState::Error;
            return false;
        }
        if (audio_chunk.sample_count > 0) {
            last_output_.audio = audio_chunk;
        }
    }

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

    if (active_mode() != core::EmuMode::GBA) {
        return true;
    }

#if !EMUGBDS_HAS_MGBA
    (void)input;
    error = "GBA input injection is unavailable because mGBA support is disabled";
    return false;
#else
    auto* gba_adapter = dynamic_cast<cores::MGbaCoreAdapter*>(active_core_);
    if (!gba_adapter) {
        return true;
    }

    mCore* native_core = gba_adapter->NativeCore();
    if (!native_core) {
        error = "mGBA core is not initialized";
        return false;
    }

    if (input.gba_press_mask != 0) {
        native_core->addKeys(native_core, input.gba_press_mask);
    }
    if (input.gba_release_mask != 0) {
        native_core->clearKeys(native_core, input.gba_release_mask);
    }

    return true;
#endif
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
    gba_av_ready_ = false;
    gba_av_supported_ = false;
    gba_video_pipeline_.Reset();
    gba_audio_pipeline_.Reset();
    ClearAvOutputs();
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
    gba_av_supported_ = dynamic_cast<cores::MGbaCoreAdapter*>(active_core_) != nullptr;
    if (gba_av_supported_ && !SetupGbaAvPipelines(error)) {
        return false;
    }
    std::cout << "[runtime] switched active core to GBA using Slot-2 ROM '"
              << slot2_rom_path_->string() << "' at frame " << frame_index_ << ".\n";
    return true;
}

bool RuntimeKernel::SetupGbaAvPipelines(std::string& error) {
#if !EMUGBDS_HAS_MGBA
    error = "GBA AV pipeline is unavailable because mGBA support is disabled";
    return false;
#else
    auto* gba_adapter = dynamic_cast<cores::MGbaCoreAdapter*>(active_core_);
    if (!gba_adapter) {
        error = "Active core is not an mGBA adapter";
        return false;
    }

    auto* native_core = gba_adapter->NativeCore();
    if (!native_core) {
        error = "mGBA core pointer is null";
        return false;
    }

    gba_video_pipeline_.Reset();
    gba_audio_pipeline_.Reset();

    if (!gba_video_pipeline_.InitializeForGba(native_core, error)) {
        return false;
    }
    // mGBA keeps a dummy renderer until reset-time renderer selection.
    // After binding a host video buffer, force one reset so GBA software
    // renderer becomes active and writes into that buffer.
    if (!gba_adapter->Reset(error)) {
        return false;
    }
    if (!gba_audio_pipeline_.InitializeForGba(native_core, 44100, 2, error)) {
        return false;
    }

    gba_av_ready_ = true;
    return true;
#endif
}

void RuntimeKernel::ClearAvOutputs() {
    last_output_.video.reset();
    last_output_.audio.reset();
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
