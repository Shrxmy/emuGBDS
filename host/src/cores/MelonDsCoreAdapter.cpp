#include "emugbds/cores/MelonDsCoreAdapter.h"

#if EMUGBDS_HAS_MELONDS
#include "Args.h"
#include "GBACart.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#endif

#include <array>
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

#if EMUGBDS_HAS_MELONDS
template <typename ImageArray>
bool ReadFixedSizeImage(
    const std::filesystem::path& path,
    ImageArray& out,
    const char* label,
    std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        error = std::string("Failed to open ") + label + ": " + path.string();
        return false;
    }

    const std::streamsize length = input.tellg();
    if (length != static_cast<std::streamsize>(out.size())) {
        error = std::string("Unexpected size for ") + label + ": " + path.string();
        return false;
    }

    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
        error = std::string("Failed to read ") + label + ": " + path.string();
        return false;
    }

    return true;
}

bool ReadFirmwareImage(const std::filesystem::path& path, melonDS::Firmware& out_firmware, std::string& error) {
    std::unique_ptr<uint8_t[]> fw_data;
    uint32_t fw_size = 0;
    if (!ReadWholeFile(path, fw_data, fw_size)) {
        error = "Failed to read DS firmware image: " + path.string();
        return false;
    }

    out_firmware = melonDS::Firmware(fw_data.get(), fw_size);
    return true;
}
#endif

} // namespace

namespace emugbds::cores {

MelonDsCoreAdapter::~MelonDsCoreAdapter() {
    Shutdown();
}

core::EmuMode MelonDsCoreAdapter::mode() const noexcept {
    return core::EmuMode::DS;
}

bool MelonDsCoreAdapter::Configure(const core::BootRequest& request, std::string& error) {
#if !EMUGBDS_HAS_MELONDS
    (void)request;
    error = "melonDS support is not compiled in";
    return false;
#else
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

    if (ds_boot_mode_ == core::DsBootMode::DsFirmware &&
        (!nds_bios7_path_.has_value() || !nds_bios9_path_.has_value() || !nds_firmware_path_.has_value())) {
        error = "DS firmware boot requires --bios-nds7, --bios-nds9, and --firmware-nds";
        return false;
    }

    return true;
#endif
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

    if (nds_bios9_path_.has_value()) {
        auto bios9 = std::make_unique<melonDS::ARM9BIOSImage>();
        if (!ReadFixedSizeImage(*nds_bios9_path_, *bios9, "NDS ARM9 BIOS", error)) {
            return false;
        }
        args.ARM9BIOS = std::move(bios9);
    }

    if (nds_bios7_path_.has_value()) {
        auto bios7 = std::make_unique<melonDS::ARM7BIOSImage>();
        if (!ReadFixedSizeImage(*nds_bios7_path_, *bios7, "NDS ARM7 BIOS", error)) {
            return false;
        }
        args.ARM7BIOS = std::move(bios7);
    }

    if (nds_firmware_path_.has_value() && !ReadFirmwareImage(*nds_firmware_path_, args.Firmware, error)) {
        return false;
    }

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
    std::string boot_error;
    const bool direct_boot = ShouldDirectBoot(boot_error);
    if (!boot_error.empty()) {
        error = std::move(boot_error);
        return false;
    }
    if (direct_boot) {
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
    std::string boot_error;
    const bool direct_boot = ShouldDirectBoot(boot_error);
    if (!boot_error.empty()) {
        error = std::move(boot_error);
        return false;
    }
    if (primary_loaded_ && primary_rom_path_.has_value() && direct_boot) {
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

bool MelonDsCoreAdapter::ShouldDirectBoot(std::string& error) const {
#if !EMUGBDS_HAS_MELONDS
    error = "melonDS support is not compiled in";
    return false;
#else
    if (!nds_) {
        error = "melonDS core is not initialized";
        return false;
    }

    switch (ds_boot_mode_) {
    case core::DsBootMode::Direct:
        return true;
    case core::DsBootMode::DsFirmware:
        if (nds_->NeedsDirectBoot()) {
            error = "DS firmware boot requested, but loaded BIOS/Firmware are not bootable; falling back to direct boot is disallowed";
            return false;
        }
        return false;
    case core::DsBootMode::Auto:
        return nds_->NeedsDirectBoot();
    }

    return nds_->NeedsDirectBoot();
#endif
}

} // namespace emugbds::cores
