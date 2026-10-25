#include "emugbds/cores/InHouseDsCoreAdapter.h"

#include <filesystem>
#include <iostream>

namespace emugbds::cores {

core::EmuMode InHouseDsCoreAdapter::mode() const noexcept {
    return core::EmuMode::DS;
}

bool InHouseDsCoreAdapter::Configure(const core::BootRequest& request, std::string& error) {
    const auto validate_if_present = [&](const std::optional<std::filesystem::path>& path, const char* label) -> bool {
        if (!path.has_value()) {
            return true;
        }
        if (std::filesystem::exists(*path)) {
            return true;
        }
        error = std::string(label) + " does not exist: " + path->string();
        return false;
    };

    if (!validate_if_present(request.nds_bios7_path, "NDS ARM7 BIOS file")) {
        return false;
    }
    if (!validate_if_present(request.nds_bios9_path, "NDS ARM9 BIOS file")) {
        return false;
    }
    if (!validate_if_present(request.nds_firmware_path, "NDS firmware file")) {
        return false;
    }

    nds_bios7_path_ = request.nds_bios7_path;
    nds_bios9_path_ = request.nds_bios9_path;
    nds_firmware_path_ = request.nds_firmware_path;
    ds_boot_mode_ = request.ds_boot_mode;
    trace_transition_ = request.trace_transition;
    configured_ = true;
    return true;
}

bool InHouseDsCoreAdapter::Initialize(std::string& error) {
    if (!configured_) {
        error = "In-house DS core is not configured";
        return false;
    }
    if (initialized_) {
        return true;
    }

    // Each session starts from a fresh core; allocating here keeps failures reportable.
    core_ = std::make_unique<inds::Core>();
    if (!core_->Configure(
            nds_bios7_path_,
            nds_bios9_path_,
            nds_firmware_path_,
            ds_boot_mode_,
            trace_transition_,
            error)) {
        return false;
    }
    if (!core_->Initialize(error)) {
        return false;
    }
    if (slot2_rom_path_.has_value() && !core_->LoadSlot2Rom(*slot2_rom_path_, error)) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool InHouseDsCoreAdapter::LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!initialized_ && !Initialize(error)) {
        return false;
    }
    if (!rom_path.empty() && !std::filesystem::exists(rom_path)) {
        error = "NDS ROM does not exist: " + rom_path.string();
        return false;
    }
    if (slot2_rom_path_.has_value() && !core_->LoadSlot2Rom(*slot2_rom_path_, error)) {
        return false;
    }
    if (!core_->LoadPrimaryRom(rom_path, error)) {
        return false;
    }

    primary_loaded_ = !rom_path.empty() || slot2_rom_path_.has_value();
    return true;
}

bool InHouseDsCoreAdapter::RunFrame(std::string& error) {
    if (!initialized_ || !primary_loaded_) {
        error = "In-house DS core is not ready to run";
        return false;
    }
    return core_->RunFrame(error);
}

bool InHouseDsCoreAdapter::Reset(std::string& error) {
    if (!initialized_) {
        error = "In-house DS core is not initialized";
        return false;
    }
    return core_->Reset(error);
}

bool InHouseDsCoreAdapter::IsRunning() const noexcept {
    return initialized_ && primary_loaded_ && core_->IsRunning();
}

void InHouseDsCoreAdapter::Shutdown() noexcept {
    std::string error;
    if (initialized_ && !core_->FlushSave(error)) {
        std::cerr << "[inds] " << error << "\n";
    }
    core_.reset(); // frees the ROM image and the console's memory
    configured_ = false;
    initialized_ = false;
    primary_loaded_ = false;
    nds_bios7_path_.reset();
    nds_bios9_path_.reset();
    nds_firmware_path_.reset();
    slot2_rom_path_.reset();
    ds_boot_mode_ = core::DsBootMode::Auto;
    trace_transition_ = false;
}

bool InHouseDsCoreAdapter::SetSecondaryRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!std::filesystem::exists(rom_path)) {
        error = "Slot-2 ROM does not exist: " + rom_path.string();
        return false;
    }

    slot2_rom_path_ = rom_path;
    if (initialized_) {
        return core_->LoadSlot2Rom(*slot2_rom_path_, error);
    }
    return true;
}

void InHouseDsCoreAdapter::ApplyInput(const runtime::InputSnapshot& input) noexcept {
    held_keys_ = (held_keys_ | input.gba_press_mask) & ~input.gba_release_mask;
    if (!core_) {
        return;
    }
    core_->SetKeys(held_keys_);
    core_->SetTouch(input.touch_down, input.touch_x, input.touch_y);
}


bool InHouseDsCoreAdapter::GetVideoFrame(runtime::VideoFrame& out_frame) const noexcept {
    if (!IsRunning()) {
        return false;
    }
    out_frame.format = runtime::PixelFormat::ABGR8888;
    out_frame.width = inds::Core::kScreenWidth;
    out_frame.height = inds::Core::kScreenHeight * 2u;
    out_frame.pixels = core_->frame_pixels();
    return true;
}


bool InHouseDsCoreAdapter::GetAudioChunk(runtime::AudioChunk& out_chunk) const noexcept {
    if (!IsRunning() || core_->audio_samples().empty()) {
        return false;
    }
    out_chunk.sample_rate = inds::Sound::kSampleRate;
    out_chunk.channels = 2u;
    out_chunk.samples = core_->audio_samples().data();
    out_chunk.sample_count = core_->audio_samples().size();
    return true;
}


std::optional<core::TransitionEvent> InHouseDsCoreAdapter::PollTransition() {
    return core_ ? core_->PollTransition() : std::nullopt;
}

} // namespace emugbds::cores
