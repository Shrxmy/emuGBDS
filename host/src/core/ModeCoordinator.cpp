#include "emugbds/core/ModeCoordinator.h"
#include "emugbds/gba/GbaCartridgeHeader.h"

#include <iostream>

namespace emugbds::core {
namespace {

const char* DsBootModeName(DsBootMode mode) {
    switch (mode) {
    case DsBootMode::Auto:
        return "auto";
    case DsBootMode::DsFirmware:
        return "ds-firmware";
    case DsBootMode::Direct:
        return "direct";
    }
    return "unknown";
}

void LogSlot2HeaderInfo(const std::filesystem::path& slot2_rom_path) {
    gba::GbaCartridgeHeaderInfo info{};
    std::string inspect_error;
    if (!gba::InspectGbaCartridgeHeader(slot2_rom_path, info, inspect_error)) {
        std::cerr << "[ModeCoordinator] Warning: " << inspect_error << '\n';
        return;
    }

    std::cout << "[ModeCoordinator] Slot-2 GBA header: title='"
              << (info.title.empty() ? "<empty>" : info.title)
              << "', game='" << (info.game_code.empty() ? "<empty>" : info.game_code)
              << "', maker='" << (info.maker_code.empty() ? "<empty>" : info.maker_code) << "'\n";

    if (!info.fixed_byte_valid) {
        std::cerr << "[ModeCoordinator] Warning: Slot-2 header fixed byte is invalid (expected 0x96).\n";
    }
    if (!info.logo_valid) {
        std::cerr << "[ModeCoordinator] Warning: Slot-2 header Nintendo logo does not match reference.\n";
    }
    if (!info.complement_valid) {
        std::cerr << "[ModeCoordinator] Warning: Slot-2 header complement check failed.\n";
    }
}

} // namespace

ModeCoordinator::ModeCoordinator(std::unique_ptr<IEmuCore> ds_core, std::unique_ptr<IEmuCore> gba_core)
    : ds_core_(std::move(ds_core)),
      gba_core_(std::move(gba_core)) {}

bool ModeCoordinator::Boot(const BootRequest& request, std::string& error) {
    const std::optional<EmuMode> resolved_mode = ResolveMode(request);
    if (!resolved_mode.has_value()) {
        error = "Unable to resolve emulation mode from the provided arguments";
        return false;
    }

    slot2_rom_path_ = request.slot2_rom_path;
    last_transition_.reset();
    trace_transition_ = request.trace_transition;
    frame_index_ = 0;

    if (*resolved_mode == EmuMode::DS) {
        if (!ds_core_) {
            error = "DS mode requested but DS core is unavailable";
            return false;
        }
        if (!request.nds_rom_path.has_value()) {
            error = "DS mode requires --nds <path>";
            return false;
        }
        if (!ds_core_->Configure(request, error)) {
            return false;
        }

        std::cout << "[ModeCoordinator] DS boot mode: " << DsBootModeName(request.ds_boot_mode) << '\n';
        if (slot2_rom_path_.has_value()) {
            LogSlot2HeaderInfo(*slot2_rom_path_);
        }

        if (!ds_core_->Initialize(error)) {
            return false;
        }
        if (slot2_rom_path_.has_value() && !ds_core_->SetSecondaryRom(*slot2_rom_path_, error)) {
            return false;
        }
        if (!ds_core_->LoadPrimaryRom(*request.nds_rom_path, error)) {
            return false;
        }
        active_core_ = ds_core_.get();
        return true;
    }

    if (!gba_core_) {
        error = "GBA mode requested but GBA core is unavailable";
        return false;
    }

    if (!gba_core_->Configure(request, error)) {
        return false;
    }

    const std::optional<std::filesystem::path> gba_path =
        request.gba_rom_path.has_value() ? request.gba_rom_path : request.slot2_rom_path;
    if (!gba_path.has_value()) {
        error = "GBA mode requires --gba <path> (or --slot2 <path> as fallback)";
        return false;
    }

    if (!gba_core_->Initialize(error)) {
        return false;
    }
    if (!gba_core_->LoadPrimaryRom(*gba_path, error)) {
        return false;
    }

    active_core_ = gba_core_.get();
    return true;
}

bool ModeCoordinator::RunFrame(std::string& error) {
    if (!active_core_) {
        error = "No active core to run";
        return false;
    }

    if (!active_core_->RunFrame(error)) {
        return false;
    }

    ++frame_index_;
    if (trace_transition_ && active_core_->mode() == EmuMode::DS) {
        std::cout << "[trace] frame=" << frame_index_ << " mode=DS\n";
    }

    std::optional<TransitionEvent> transition = active_core_->PollTransition();
    if (!transition.has_value()) {
        return true;
    }

    std::cout << "[ModeCoordinator] Transition requested at frame " << frame_index_
              << ": " << transition->reason << '\n';
    last_transition_ = transition;

    if (transition->from_mode == EmuMode::DS && transition->to_mode == EmuMode::GBA) {
        return SwitchToGbaFromSlot2(error);
    }

    return true;
}

EmuMode ModeCoordinator::ActiveMode() const noexcept {
    if (!active_core_) {
        return EmuMode::DS;
    }
    return active_core_->mode();
}

IEmuCore* ModeCoordinator::ActiveCore() const noexcept {
    return active_core_;
}

const std::optional<TransitionEvent>& ModeCoordinator::LastTransition() const noexcept {
    return last_transition_;
}

std::optional<EmuMode> ModeCoordinator::ResolveMode(const BootRequest& request) {
    switch (request.mode_hint) {
    case ModeHint::DS:
        return EmuMode::DS;
    case ModeHint::GBA:
        return EmuMode::GBA;
    case ModeHint::Auto:
        break;
    }

    if (request.nds_rom_path.has_value()) {
        return EmuMode::DS;
    }
    if (request.gba_rom_path.has_value() || request.slot2_rom_path.has_value()) {
        return EmuMode::GBA;
    }
    return std::nullopt;
}

bool ModeCoordinator::SwitchToGbaFromSlot2(std::string& error) {
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
    std::cout << "[ModeCoordinator] Switched active core to GBA using Slot-2 ROM '"
              << slot2_rom_path_->string() << "' at frame " << frame_index_ << ".\n";
    return true;
}

} // namespace emugbds::core
