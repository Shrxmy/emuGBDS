#include "emugbds/inds/Core.h"

#include "emugbds/arm/CpuConstants.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

namespace emugbds::inds {
namespace {

constexpr uint64_t kFirmwareSlot2TransitionFrames = 2u;
// One frame is 263 lines x 355 dots x 6 cycles of the 33.51 MHz ARM7 clock;
// the ARM9 runs at twice that rate.
constexpr uint32_t kArm7CyclesPerFrame = 560190u;
constexpr uint32_t kSliceCycles = 32u;

constexpr uint32_t kHeaderCopyAddress = 0x027FFE00u;
constexpr uint32_t kHeaderCopySize = 0x170u;
constexpr uint32_t kSecureAreaOffset = 0x4000u;
constexpr uint32_t kDestroyedSecureAreaId = 0xE7FFDEFFu;
constexpr uint32_t kBootDtcmBase = 0x027C0000u; // Above the 32 MiB ITCM window, below the header copy.

constexpr uint32_t kCpsrSvc = 0xD3u; // IRQ/FIQ disabled, ARM state
constexpr uint32_t kCpsrIrq = 0xD2u;
constexpr uint32_t kCpsrSys = 0xDFu;

struct BootStacks {
    uint32_t svc;
    uint32_t irq;
    uint32_t sys;
};

// Enters `entry` in System mode with the banked stacks the firmware leaves behind.
void StartCpu(arm::ArmCpu& cpu, uint32_t entry, const BootStacks& stacks) {
    cpu.WriteCpsr(kCpsrSvc);
    cpu.state().r[13] = stacks.svc;
    cpu.WriteCpsr(kCpsrIrq);
    cpu.state().r[13] = stacks.irq;
    cpu.WriteCpsr(kCpsrSys);
    cpu.state().r[13] = stacks.sys;
    cpu.state().r[15] = entry & ~3u;
}

} // namespace

bool Core::CpuHost::OnSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) {
    (void)swi_imm;
    (void)from_thumb;
    (void)error;
    if (arm9_ ? !core_.bus_.HasBios9() : !core_.bus_.HasBios7()) {
        return false; // No BIOS emulation yet: SWIs need the BIOS images.
    }
    // A Thumb SWI leaves LR at the next instruction; the BIOS reads the SWI number from [LR-2].
    const uint32_t vector_base = arm9_ ? core_.bus_.cp15().exception_base() : 0u;
    (arm9_ ? core_.arm9_ : core_.arm7_)
        .EnterException(arm::kCpuModeSupervisor, vector_base + arm::kSwiVectorAddress, false, next_pc, true);
    return true;
}


bool Core::CpuHost::OnCoprocessorRead(uint32_t coprocessor, uint32_t opcode1, uint32_t crn, uint32_t crm,
                                      uint32_t opcode2, uint32_t& value) {
    return arm9_ && coprocessor == 15u && core_.bus_.cp15().Read(opcode1, crn, crm, opcode2, value);
}


bool Core::CpuHost::OnCoprocessorWrite(uint32_t coprocessor, uint32_t opcode1, uint32_t crn, uint32_t crm,
                                       uint32_t opcode2, uint32_t value) {
    if (!arm9_ || coprocessor != 15u || !core_.bus_.cp15().Write(opcode1, crn, crm, opcode2, value)) {
        return false;
    }
    core_.arm9_.set_load_interworking(core_.bus_.cp15().load_interworking());
    return true;
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

    if (cpus_active_) {
        arm9_.set_trace_frame(frame_counter_);
        arm7_.set_trace_frame(frame_counter_);
        // shortcut: one instruction per cycle on both CPUs at the fixed 2:1 clock ratio, interleaved in
        // short slices; replace with per-instruction timing once games depend on it.
        for (uint32_t done = 0u; done < kArm7CyclesPerFrame; done += kSliceCycles) {
            const uint32_t slice = std::min(kSliceCycles, kArm7CyclesPerFrame - done);
            for (uint32_t i = 0u; i < slice * 2u && !arm9_halted_; ++i) {
                if (!arm9_.Step(error)) {
                    return false;
                }
                arm9_halted_ = bus_.cp15().ConsumeHaltRequest(); // Nothing wakes it until IRQs exist.
            }
            for (uint32_t i = 0u; i < slice; ++i) {
                if (!arm7_.Step(error)) {
                    return false;
                }
            }
        }
    }

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
    bus_.ResetMemory();
    arm9_.Reset();
    arm7_.Reset();
    arm9_.set_load_interworking(true);
    arm9_.WriteCpsr(kCpsrSvc);
    arm7_.WriteCpsr(kCpsrSvc);
    arm9_halted_ = false;
    cpus_active_ = false;
    pending_transition_.reset();
    firmware_menu_frame_counter_ = 0;

    if (nds_header_.has_value() && nds_rom_loaded_) {
        // Any loaded cartridge is direct-booted: the firmware menu is not emulated.
        if (!DirectBoot(error)) {
            running_ = false;
            return false;
        }
        cpus_active_ = true;
    } else {
        arm9_.state().r[15] = bus_.HasBios9() ? MemoryBus::kArm9BiosBase : MemoryBus::kMainRamBase;
        arm7_.state().r[15] = bus_.HasBios7() ? 0u : MemoryBus::kMainRamBase;
    }

    running_ = configured_;
    return true;
}

bool Core::DirectBoot(std::string& error) {
    const std::vector<uint8_t>& rom = bus_.nds_rom();
    const nds::NdsCartridgeHeaderInfo& header = *nds_header_;
    const auto copy = [&](arm::IBus& bus, uint32_t rom_offset, uint32_t ram_address, uint32_t size, const char* what) {
        if (rom_offset > rom.size() || size > rom.size() - rom_offset) {
            error = std::string("NDS ") + what + " binary lies outside the ROM image";
            return false;
        }
        for (uint32_t i = 0u; i < size; ++i) {
            bus.Write8(ram_address + i, rom[rom_offset + i]);
        }
        return true;
    };

    // shortcut: approximates the firmware's CP15 handoff (TCMs on, DTCM above the ITCM window,
    // high vectors); boot code reprograms CP15 itself, so this only has to be usable.
    Cp15& cp15 = bus_.cp15();
    cp15.Write(0u, 9u, 1u, 1u, 0x00000020u);             // ITCM: 32 MiB virtual size from 0
    cp15.Write(0u, 9u, 1u, 0u, kBootDtcmBase | 0x0Au);   // DTCM: 16 KiB, outside the ITCM window
    cp15.Write(0u, 1u, 0u, 0u, cp15.control() | (1u << 16) | (1u << 18));

    if (!copy(bus_.arm9(), 0u, kHeaderCopyAddress, std::min<uint32_t>(kHeaderCopySize, static_cast<uint32_t>(rom.size())), "header") ||
        !copy(bus_.arm9(), header.arm9_rom_offset, header.arm9_ram_address, header.arm9_size, "ARM9") ||
        !copy(bus_.arm7(), header.arm7_rom_offset, header.arm7_ram_address, header.arm7_size, "ARM7")) {
        return false;
    }
    // The BIOS verifies a decrypted secure area's "encryObj" ID and then destroys it.
    if (header.arm9_rom_offset == kSecureAreaOffset && rom.size() >= kSecureAreaOffset + 8u &&
        std::memcmp(&rom[kSecureAreaOffset], "encryObj", 8u) == 0) {
        bus_.arm9().Write32(header.arm9_ram_address, kDestroyedSecureAreaId);
        bus_.arm9().Write32(header.arm9_ram_address + 4u, kDestroyedSecureAreaId);
    }

    // shortcut: approximate firmware stack tops (DTCM for the ARM9, ARM7 WRAM for the ARM7);
    // startup code sets its own stacks before relying on them.
    StartCpu(arm9_, header.arm9_entry_address, {kBootDtcmBase + 0x3FC0u, kBootDtcmBase + 0x3FA0u, kBootDtcmBase + 0x3EC0u});
    StartCpu(arm7_, header.arm7_entry_address, {0x0380FFDCu, 0x0380FFB0u, 0x0380FF00u});
    return true;
}

std::optional<core::TransitionEvent> Core::PollTransition() {
    std::optional<core::TransitionEvent> event = pending_transition_;
    pending_transition_.reset();
    return event;
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
