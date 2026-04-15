#include "emugbds/cores/MelonDsCoreAdapter.h"

#if EMUGBDS_HAS_MELONDS
#include "Args.h"
#include "GBACart.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#endif

#include <cstring>
#include <fstream>
#include <iterator>

namespace {

bool ReadWholeFile(const std::filesystem::path& path, std::unique_ptr<uint8_t[]>& out_data, uint32_t& out_size) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        return false;
    }

    const std::streamsize length = input.tellg();
    if (length <= 0) {
        return false;
    }

    input.seekg(0, std::ios::beg);
    auto buffer = std::make_unique<uint8_t[]>(static_cast<std::size_t>(length));
    if (!input.read(reinterpret_cast<char*>(buffer.get()), length)) {
        return false;
    }

    out_size = static_cast<uint32_t>(length);
    out_data = std::move(buffer);
    return true;
}

} // namespace

namespace emugbds::cores {

MelonDsCoreAdapter::~MelonDsCoreAdapter() {
    Shutdown();
}

core::EmuMode MelonDsCoreAdapter::mode() const noexcept {
    return core::EmuMode::DS;
}

bool MelonDsCoreAdapter::Initialize(std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    error = "melonDS support is not compiled in";
    return false;
#else
    if (nds_) {
        return true;
    }

    melonDS::NDSArgs args;
    nds_ = std::make_unique<melonDS::NDS>(std::move(args), this);
    nds_->Reset();
    return true;
#endif
}

bool MelonDsCoreAdapter::LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    (void)rom_path;
    error = "melonDS support is not compiled in";
    return false;
#else
    if (!nds_ && !Initialize(error)) {
        return false;
    }

    std::unique_ptr<uint8_t[]> rom_data;
    uint32_t rom_size = 0;
    if (!ReadWholeFile(rom_path, rom_data, rom_size)) {
        error = "Failed to read DS ROM: " + rom_path.string();
        return false;
    }

    auto cart = melonDS::NDSCart::ParseROM(std::move(rom_data), rom_size, this);
    if (!cart) {
        error = "melonDS failed to parse DS ROM: " + rom_path.string();
        return false;
    }

    nds_->SetNDSCart(std::move(cart));

    if (!InsertSlot2Rom(error)) {
        return false;
    }

    nds_->Reset();
    if (nds_->NeedsDirectBoot()) {
        nds_->SetupDirectBoot(rom_path.filename().string());
    }
    nds_->Start();

    primary_loaded_ = true;
    primary_rom_path_ = rom_path;
    pending_transition_.reset();
    return true;
#endif
}

bool MelonDsCoreAdapter::RunFrame(std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    error = "melonDS support is not compiled in";
    return false;
#else
    if (!nds_ || !primary_loaded_) {
        error = "melonDS core is not ready to run";
        return false;
    }

    if (!nds_->IsRunning()) {
        if (!pending_transition_.has_value()) {
            error = "melonDS is not running";
            return false;
        }
        return true;
    }

    nds_->RunFrame();
    return true;
#endif
}

bool MelonDsCoreAdapter::Reset(std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    error = "melonDS support is not compiled in";
    return false;
#else
    if (!nds_) {
        error = "melonDS core is not initialized";
        return false;
    }

    nds_->Reset();
    if (primary_loaded_ && primary_rom_path_.has_value() && nds_->NeedsDirectBoot()) {
        nds_->SetupDirectBoot(primary_rom_path_->filename().string());
    }
    nds_->Start();
    return true;
#endif
}

bool MelonDsCoreAdapter::IsRunning() const noexcept {
#if !EMUGBDS_HAS_MELONDS
    return false;
#else
    return nds_ != nullptr && nds_->IsRunning();
#endif
}

void MelonDsCoreAdapter::Shutdown() noexcept {
#if EMUGBDS_HAS_MELONDS
    nds_.reset();
    primary_loaded_ = false;
    primary_rom_path_.reset();
    pending_transition_.reset();
#endif
}

bool MelonDsCoreAdapter::SetSecondaryRom(const std::filesystem::path& rom_path, std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    (void)rom_path;
    error = "melonDS support is not compiled in";
    return false;
#else
    slot2_rom_path_ = rom_path;
    if (!primary_loaded_) {
        return true;
    }
    return InsertSlot2Rom(error);
#endif
}

std::optional<core::TransitionEvent> MelonDsCoreAdapter::PollTransition() {
    std::optional<core::TransitionEvent> event = pending_transition_;
    pending_transition_.reset();
    return event;
}

void MelonDsCoreAdapter::OnSignalStop(melonDS::Platform::StopReason reason) {
#if EMUGBDS_HAS_MELONDS
    if (reason == melonDS::Platform::StopReason::GBAModeNotSupported) {
        core::TransitionEvent event;
        event.from_mode = core::EmuMode::DS;
        event.to_mode = core::EmuMode::GBA;
        event.reason = "DS firmware requested native GBA mode";
        pending_transition_ = std::move(event);
    }
#else
    (void)reason;
#endif
}

void MelonDsCoreAdapter::OnWriteNdsSave(const uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        return;
    }
    nds_save_image_.assign(data, data + size);
}

void MelonDsCoreAdapter::OnWriteGbaSave(const uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        return;
    }
    gba_save_image_.assign(data, data + size);
}

void MelonDsCoreAdapter::OnWriteFirmware(const uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        return;
    }
    firmware_image_.assign(data, data + size);
}

bool MelonDsCoreAdapter::InsertSlot2Rom(std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    error = "melonDS support is not compiled in";
    return false;
#else
    if (!slot2_rom_path_.has_value()) {
        return true;
    }

    std::unique_ptr<uint8_t[]> rom_data;
    uint32_t rom_size = 0;
    if (!ReadWholeFile(*slot2_rom_path_, rom_data, rom_size)) {
        error = "Failed to read Slot-2 ROM: " + slot2_rom_path_->string();
        return false;
    }

    auto cart = melonDS::GBACart::ParseROM(std::move(rom_data), rom_size, this);
    if (!cart) {
        error = "melonDS failed to parse Slot-2 ROM: " + slot2_rom_path_->string();
        return false;
    }

    nds_->SetGBACart(std::move(cart));
    return true;
#endif
}

} // namespace emugbds::cores
