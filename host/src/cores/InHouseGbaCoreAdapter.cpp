#include "emugbds/cores/InHouseGbaCoreAdapter.h"

namespace emugbds::cores {

core::EmuMode InHouseGbaCoreAdapter::mode() const noexcept {
    return core::EmuMode::GBA;
}

bool InHouseGbaCoreAdapter::Initialize(std::string& error) {
    if (initialized_) {
        return true;
    }

    if (!core_.Initialize(error)) {
        return false;
    }
    initialized_ = true;

    if (bios_path_.has_value() && !core_.LoadBios(*bios_path_, error)) {
        initialized_ = false;
        return false;
    }

    return true;
}

bool InHouseGbaCoreAdapter::LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!initialized_ && !Initialize(error)) {
        return false;
    }
    if (!core_.LoadRom(rom_path, error)) {
        return false;
    }
    loaded_ = true;
    return true;
}

bool InHouseGbaCoreAdapter::RunFrame(std::string& error) {
    if (!initialized_ || !loaded_) {
        error = "In-house GBA core is not ready to run";
        return false;
    }
    return core_.RunFrame(error);
}

bool InHouseGbaCoreAdapter::Reset(std::string& error) {
    if (!initialized_ || !loaded_) {
        error = "In-house GBA core is not ready to reset";
        return false;
    }
    return core_.Reset(error);
}

bool InHouseGbaCoreAdapter::IsRunning() const noexcept {
    return initialized_ && loaded_ && core_.IsRunning();
}

void InHouseGbaCoreAdapter::Shutdown() noexcept {
    initialized_ = false;
    loaded_ = false;
    core_ = {};
}

bool InHouseGbaCoreAdapter::SetBiosPath(const std::filesystem::path& bios_path, std::string& error) {
    if (!std::filesystem::exists(bios_path)) {
        error = "GBA BIOS file does not exist: " + bios_path.string();
        return false;
    }
    bios_path_ = bios_path;
    if (initialized_) {
        return core_.LoadBios(*bios_path_, error);
    }
    return true;
}

} // namespace emugbds::cores
