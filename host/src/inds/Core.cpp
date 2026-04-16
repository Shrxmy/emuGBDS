#include "emugbds/inds/Core.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

namespace emugbds::inds {
namespace {

constexpr uint32_t kArm9CyclesPerFrame = 560190u;
constexpr uint32_t kArm7CyclesPerFrame = 280095u;
constexpr uint32_t kArmInstructionsPerCycle = 1u;
constexpr uint64_t kFirmwareSlot2TransitionFrames = 2u;

constexpr uint32_t kMainRamBase = 0x02000000u;
constexpr uint32_t kMainRamSize = 0x00400000u;
constexpr uint32_t kSharedWramBase = 0x03000000u;
constexpr uint32_t kSharedWramSize = 0x00010000u;
constexpr uint32_t kSlot2Base = 0x08000000u;
constexpr uint32_t kArm9BiosBase = 0xFFFF0000u;
constexpr uint32_t kArm7BiosBase = 0x00000000u;

bool LoadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open file: " + path.string();
        return false;
    }

    out_bytes.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (out_bytes.empty()) {
        error = "File is empty: " + path.string();
        return false;
    }
    return true;
}

uint8_t ReadFromSpan(const std::vector<uint8_t>& bytes, uint32_t base, uint32_t address) {
    if (bytes.empty() || address < base) {
        return 0;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return 0;
    }
    return bytes[offset];
}

void WriteToSpan(std::vector<uint8_t>& bytes, uint32_t base, uint32_t address, uint8_t value) {
    if (bytes.empty() || address < base) {
        return;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return;
    }
    bytes[offset] = value;
}

} // namespace

bool MemoryBus::LoadBios7(const std::filesystem::path& bios_path, std::string& error) {
    return LoadWholeFile(bios_path, bios7_, error);
}

bool MemoryBus::LoadBios9(const std::filesystem::path& bios_path, std::string& error) {
    return LoadWholeFile(bios_path, bios9_, error);
}

bool MemoryBus::LoadFirmware(const std::filesystem::path& firmware_path, std::string& error) {
    return LoadWholeFile(firmware_path, firmware_, error);
}

bool MemoryBus::LoadNdsRom(const std::filesystem::path& rom_path, std::string& error) {
    return LoadWholeFile(rom_path, nds_rom_, error);
}

bool MemoryBus::LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error) {
    return LoadWholeFile(rom_path, slot2_rom_, error);
}

void MemoryBus::ClearNdsRom() {
    nds_rom_.clear();
}

void MemoryBus::ClearSlot2Rom() {
    slot2_rom_.clear();
}

uint8_t MemoryBus::Read8(uint32_t address) const {
    if (address >= kMainRamBase && address < kMainRamBase + kMainRamSize) {
        return ReadFromSpan(main_ram_, kMainRamBase, address);
    }
    if (address >= kSharedWramBase && address < kSharedWramBase + kSharedWramSize) {
        return ReadFromSpan(shared_wram_, kSharedWramBase, address);
    }
    if (address >= kArm9BiosBase && !bios9_.empty()) {
        const uint32_t offset = (address - kArm9BiosBase) % static_cast<uint32_t>(bios9_.size());
        return bios9_[offset];
    }
    if (address < static_cast<uint32_t>(bios7_.size()) && !bios7_.empty()) {
        return bios7_[address];
    }
    if (address >= kSlot2Base && !slot2_rom_.empty()) {
        const uint32_t offset = (address - kSlot2Base) % static_cast<uint32_t>(slot2_rom_.size());
        return slot2_rom_[offset];
    }
    return 0;
}

uint16_t MemoryBus::Read16(uint32_t address) const {
    const uint16_t lo = Read8(address);
    const uint16_t hi = Read8(address + 1);
    return static_cast<uint16_t>(lo | (hi << 8));
}

uint32_t MemoryBus::Read32(uint32_t address) const {
    const uint32_t b0 = Read8(address);
    const uint32_t b1 = Read8(address + 1);
    const uint32_t b2 = Read8(address + 2);
    const uint32_t b3 = Read8(address + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void MemoryBus::Write8(uint32_t address, uint8_t value) {
    if (main_ram_.empty()) {
        main_ram_.assign(kMainRamSize, 0);
    }
    if (shared_wram_.empty()) {
        shared_wram_.assign(kSharedWramSize, 0);
    }
    if (address >= kMainRamBase && address < kMainRamBase + kMainRamSize) {
        WriteToSpan(main_ram_, kMainRamBase, address, value);
        return;
    }
    if (address >= kSharedWramBase && address < kSharedWramBase + kSharedWramSize) {
        WriteToSpan(shared_wram_, kSharedWramBase, address, value);
    }
}

void MemoryBus::Write16(uint32_t address, uint16_t value) {
    Write8(address, static_cast<uint8_t>(value & 0xFFu));
    Write8(address + 1, static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void MemoryBus::Write32(uint32_t address, uint32_t value) {
    Write8(address, static_cast<uint8_t>(value & 0xFFu));
    Write8(address + 1, static_cast<uint8_t>((value >> 8) & 0xFFu));
    Write8(address + 2, static_cast<uint8_t>((value >> 16) & 0xFFu));
    Write8(address + 3, static_cast<uint8_t>((value >> 24) & 0xFFu));
}

bool Core::Configure(
    const std::optional<std::filesystem::path>& nds_bios7_path,
    const std::optional<std::filesystem::path>& nds_bios9_path,
    const std::optional<std::filesystem::path>& nds_firmware_path,
    core::DsBootMode boot_mode,
    bool trace_transition,
    std::string& error) {
    if (nds_bios7_path.has_value() && !bus_.LoadBios7(*nds_bios7_path, error)) {
        return false;
    }
    if (nds_bios9_path.has_value() && !bus_.LoadBios9(*nds_bios9_path, error)) {
        return false;
    }
    if (nds_firmware_path.has_value() && !bus_.LoadFirmware(*nds_firmware_path, error)) {
        return false;
    }

    boot_mode_ = boot_mode;
    trace_transition_ = trace_transition;
    configured_ = true;
    return true;
}

bool Core::Initialize(std::string& error) {
    (void)error;
    initialized_ = true;
    frame_counter_ = 0;
    firmware_menu_frame_counter_ = 0;
    pending_transition_.reset();
    return Reset(error);
}

bool Core::LoadPrimaryRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!rom_path.empty()) {
        nds::NdsCartridgeHeaderInfo header{};
        if (!nds::InspectNdsCartridgeHeader(rom_path, header, error)) {
            return false;
        }
        if (!bus_.LoadNdsRom(rom_path, error)) {
            return false;
        }

        nds_header_ = std::move(header);
        nds_rom_loaded_ = true;
    } else {
        bus_.ClearNdsRom();
        nds_header_.reset();
        nds_rom_loaded_ = false;
    }

    if (!initialized_ && !Initialize(error)) {
        return false;
    }
    return Reset(error);
}

bool Core::LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error) {
    gba::GbaCartridgeHeaderInfo header{};
    if (!gba::InspectGbaCartridgeHeader(rom_path, header, error)) {
        return false;
    }
    if (!bus_.LoadSlot2Rom(rom_path, error)) {
        return false;
    }

    slot2_header_ = std::move(header);
    slot2_rom_loaded_ = true;
    return true;
}

bool Core::RunFrame(std::string& error) {
    if (!initialized_) {
        error = "In-house DS core is not initialized";
        return false;
    }
    if (!running_) {
        error = "In-house DS core is not running";
        return false;
    }

    const uint32_t arm9_instructions = kArm9CyclesPerFrame * kArmInstructionsPerCycle;
    const uint32_t arm7_instructions = kArm7CyclesPerFrame * kArmInstructionsPerCycle;
    arm9_.r[15] = AdvancePc(arm9_.r[15], arm9_instructions, arm9_.thumb);
    arm7_.r[15] = AdvancePc(arm7_.r[15], arm7_instructions, arm7_.thumb);

    ++frame_counter_;

    if (ShouldRequestFirmwareSlot2Transition()) {
        ++firmware_menu_frame_counter_;
        if (firmware_menu_frame_counter_ >= kFirmwareSlot2TransitionFrames && !pending_transition_.has_value()) {
            core::TransitionEvent event;
            event.from_mode = core::EmuMode::DS;
            event.to_mode = core::EmuMode::GBA;
            event.reason = "In-house DS firmware selected Slot-2 GBA cartridge boot";
            pending_transition_ = event;
            if (trace_transition_) {
                std::cout << "[inds] queued transition to GBA after firmware Slot-2 selection\n";
            }
        }
    } else {
        firmware_menu_frame_counter_ = 0;
    }

    return true;
}

bool Core::Reset(std::string& error) {
    (void)error;
    arm9_ = {};
    arm7_ = {};
    arm9_.cpsr = 0x0000001Fu;
    arm7_.cpsr = 0x0000001Fu;
    arm9_.thumb = false;
    arm7_.thumb = false;

    if (nds_header_.has_value()) {
        arm9_.r[15] = nds_header_->arm9_entry_address;
        arm7_.r[15] = nds_header_->arm7_entry_address;
    } else {
        arm9_.r[15] = bus_.HasBios9() ? kArm9BiosBase : kMainRamBase;
        arm7_.r[15] = bus_.HasBios7() ? kArm7BiosBase : kMainRamBase;
    }

    pending_transition_.reset();
    firmware_menu_frame_counter_ = 0;
    running_ = configured_;
    return true;
}

std::optional<core::TransitionEvent> Core::PollTransition() {
    std::optional<core::TransitionEvent> event = pending_transition_;
    pending_transition_.reset();
    return event;
}

uint32_t Core::AdvancePc(uint32_t pc, uint32_t instructions, bool thumb) {
    const uint32_t bytes_per_instruction = thumb ? 2u : 4u;
    return pc + instructions * bytes_per_instruction;
}

bool Core::ShouldRequestFirmwareSlot2Transition() const noexcept {
    if (boot_mode_ != core::DsBootMode::DsFirmware) {
        return false;
    }
    if (!slot2_rom_loaded_ || nds_rom_loaded_) {
        return false;
    }
    if (!slot2_header_.has_value()) {
        return false;
    }
    // Some homebrew and patched ROMs may fail strict header checks, so only require header parse success.
    return true;
}

} // namespace emugbds::inds
