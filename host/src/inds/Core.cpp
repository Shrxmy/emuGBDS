#include "emugbds/inds/Core.h"

#include "emugbds/core/SaveFile.h"

#include "emugbds/arm/CpuConstants.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace emugbds::inds {
namespace {

constexpr uint64_t kFirmwareSlot2TransitionFrames = 2u;
// One frame is 263 lines x 355 dots x 6 cycles of the 33.51 MHz ARM7 clock;
// the ARM9 runs at twice that rate.
constexpr uint32_t kArm7CyclesPerFrame = 560190u;
constexpr uint32_t kCyclesPerLine = 2130u;   // 355 dots x 6
constexpr uint32_t kHBlankStartCycle = 1536u; // after 256 visible dots
constexpr uint32_t kSliceCycles = 32u;
constexpr uint32_t kIrqVectorOffset = 0x18u;

constexpr uint32_t kHeaderCopyAddress = 0x027FFE00u;
constexpr uint32_t kHeaderCopySize = 0x170u;
constexpr uint32_t kSecureAreaOffset = 0x4000u;
constexpr uint32_t kDestroyedSecureAreaId = 0xE7FFDEFFu;
constexpr uint32_t kBootDtcmBase = 0x027C0000u; // Above the 32 MiB ITCM window, below the header copy.

// Offset of the current firmware user settings (the copy with the newer 7-bit update
// counter, among the two at the end of the image), or 0 when the image is too small.
size_t CurrentUserSettings(const std::vector<uint8_t>& firmware) {
    if (firmware.size() < 0x20000u) {
        return 0u;
    }
    const size_t first = firmware.size() - 0x200u;
    const uint32_t count0 = firmware[first + 0x70u] | (firmware[first + 0x71u] << 8u);
    const uint32_t count1 = firmware[first + 0x170u] | (firmware[first + 0x171u] << 8u);
    return first + (((count1 - count0) & 0x7Fu) == 1u ? 0x100u : 0u);
}

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
    if (!FlushSave(error)) {
        return false; // Keep the previous game's unsaved changes rather than dropping them.
    }
    if (!rom_path.empty()) {
        nds::NdsCartridgeHeaderInfo header{};
        if (!nds::InspectNdsCartridgeHeader(rom_path, header, error)) {
            return false;
        }
        if (!bus_.LoadNdsRom(rom_path, error)) {
            return false;
        }
        std::filesystem::path save_path = rom_path;
        save_path.replace_extension(".sav");
        if (!bus_.io().card().backup().Load(save_path, error)) {
            return false;
        }

        nds_header_ = std::move(header);
        nds_rom_loaded_ = true;
    } else {
        bus_.ClearNdsRom();
        bus_.io().card().backup().Clear();
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

    bus_.io().sound().ClearSamples();
    if (cpus_active_) {
        arm9_.set_trace_frame(frame_counter_);
        arm7_.set_trace_frame(frame_counter_);
        Io& io = bus_.io();
        io.StartLine(0u);
        // shortcut: one instruction per cycle on both CPUs at the fixed 2:1 clock ratio, interleaved in
        // short slices; replace with per-instruction timing once games depend on it.
        for (uint32_t done = 0u; done < kArm7CyclesPerFrame; done += kSliceCycles) {
            const uint32_t slice = std::min(kSliceCycles, kArm7CyclesPerFrame - done);
            if (!RunCpu(arm9_, 0, slice * 2u, error) ||
                (!io.gba_mode_requested() && !RunCpu(arm7_, 1, slice, error))) {
                return false;
            }
            if (io.ConsumeGbaModeRequest()) {
                // The ARM7 asked the hardware to restart as a GBA (HALTCNT power-down mode 1).
                core::TransitionEvent event;
                event.from_mode = core::EmuMode::DS;
                event.to_mode = core::EmuMode::GBA;
                event.reason = "DS ARM7 entered GBA mode (HALTCNT)";
                pending_transition_ = event;
                cpus_active_ = false;
                // The GBA core reads the Slot-2 save from disk next: write ours first, then let go of
                // it, so the DS side never flushes a stale copy over what the GBA game saves later.
                std::string save_error;
                if (!FlushSave(save_error)) {
                    std::cerr << "[inds] " << save_error << "\n";
                }
                bus_.slot2_backup() = ingba::Backup{};
                if (trace_transition_) {
                    std::cout << "[inds] ARM7 wrote HALTCNT GBA mode; switching to GBA\n";
                }
                break;
            }
            // shortcut: timers advance once per slice with their end-of-slice settings, so starting or
            // stopping one mid-slice is off by up to kSliceCycles; sync on timer access if a game needs it.
            io.AdvanceTimers(slice);
            io.sound().Advance(slice);
            AdvanceLcd(done, done + slice);
        }
    }

    ++frame_counter_;
    DumpFrameIfRequested();
    FlushSaveWhenIdle();

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

bool Core::RunCpu(arm::ArmCpu& cpu, int index, uint32_t steps, std::string& error) {
    Io& io = bus_.io();
    bool& halted = index == 0 ? arm9_halted_ : arm7_halted_;
    for (uint32_t i = 0u; i < steps; ++i) {
        if (halted) {
            if (!io.WakeRequested(index)) {
                return true; // Stays halted for the rest of this slice.
            }
            halted = false;
        }
        arm::CpuState& state = cpu.state();
        if ((state.cpsr & arm::kCpsrIrqDisableBit) == 0u && io.IrqAsserted(index)) {
            // The IRQ is taken before the instruction at r15; the BIOS returns with SUBS PC, LR, #4.
            const uint32_t vector_base = index == 0 ? bus_.cp15().exception_base() : 0u;
            cpu.EnterException(arm::kCpuModeIrq, vector_base + kIrqVectorOffset, false, state.r[15] + 4u, true);
        }
        if (!cpu.Step(error)) {
            return false;
        }
        halted = index == 0 ? bus_.cp15().ConsumeHaltRequest() : io.ConsumeArm7HaltRequest();
        if (io.gba_mode_requested()) {
            return true; // The DS stops at the instruction that requested GBA mode.
        }
    }
    return true;
}


void Core::SetTouch(bool down, uint32_t x, uint32_t y) {
    uint16_t adc_x = 0u;
    uint16_t adc_y = 0u;
    if (down) {
        // Invert the firmware's ADC-to-pixel mapping from its two calibration points (user settings 58h-63h).
        const std::vector<uint8_t>& firmware = bus_.firmware_image();
        int32_t adc1[2] = {0, 0}, adc2[2] = {4095, 4095}, scr1[2] = {0, 0}, scr2[2] = {255, 191};
        if (const size_t user = CurrentUserSettings(firmware)) {
            const auto read16 = [&](size_t at) { return static_cast<int32_t>(firmware[at] | (firmware[at + 1u] << 8u)); };
            const int32_t cal[8] = {read16(user + 0x58u), read16(user + 0x5Au), firmware[user + 0x5Cu], firmware[user + 0x5Du],
                read16(user + 0x5Eu), read16(user + 0x60u), firmware[user + 0x62u], firmware[user + 0x63u]};
            if (cal[0] != cal[4] && cal[1] != cal[5] && cal[2] != cal[6] && cal[3] != cal[7]) {
                adc1[0] = cal[0]; adc1[1] = cal[1]; scr1[0] = cal[2]; scr1[1] = cal[3];
                adc2[0] = cal[4]; adc2[1] = cal[5]; scr2[0] = cal[6]; scr2[1] = cal[7];
            }
        }
        const auto to_adc = [&](uint32_t axis, uint32_t pixel) {
            const int32_t adc = adc1[axis] + (static_cast<int32_t>(pixel) - scr1[axis]) * (adc2[axis] - adc1[axis]) / (scr2[axis] - scr1[axis]);
            return static_cast<uint16_t>(std::clamp(adc, 0, 4095));
        };
        adc_x = to_adc(0u, std::min(x, kScreenWidth - 1u));
        adc_y = to_adc(1u, std::min(y, kScreenHeight - 1u));
    }
    bus_.io().SetTouch(down, adc_x, adc_y);
}


void Core::RenderLine(uint32_t line) {
    const uint16_t powcnt = bus_.io().powcnt(0);
    uint32_t* top = &frame_[line * kScreenWidth];
    uint32_t* bottom = &frame_[(kScreenHeight + line) * kScreenWidth];
    if ((powcnt & 1u) == 0u) {
        std::fill(top, top + kScreenWidth, 0xFF000000u); // LCDs off
        std::fill(bottom, bottom + kScreenWidth, 0xFF000000u);
        return;
    }
    const bool a_on_top = (powcnt & 0x8000u) != 0u; // POWCNT1 display swap
    bus_.engine(0).RenderLine(line, (powcnt & 2u) != 0u, a_on_top ? top : bottom);
    bus_.engine(1).RenderLine(line, (powcnt & 0x200u) != 0u, a_on_top ? bottom : top);
}


void Core::FlushSaveWhenIdle() {
    // Save after the game has stopped writing for a second, so bursts of page writes cost one file write.
    constexpr uint32_t kIdleFrames = 60u;
    const Backup& backup = bus_.io().card().backup();
    const ingba::Backup& slot2 = bus_.slot2_backup();
    const uint64_t firmware = bus_.io().firmware_revision();
    const uint64_t revision = backup.revision() + slot2.revision() + firmware; // all only ever grow
    if (revision != save_revision_) {
        save_revision_ = revision;
        save_idle_frames_ = 0u;
    } else if ((backup.dirty() || slot2.dirty() || firmware != firmware_flushed_revision_) &&
               ++save_idle_frames_ >= kIdleFrames) {
        std::string error;
        if (!FlushSave(error)) {
            std::cerr << "[inds] " << error << "\n";
            save_idle_frames_ = 0u; // retry after another quiet second
        }
    }
}


bool Core::FlushSave(std::string& error) {
    // Both saves are attempted; the first error is reported.
    std::string slot2_error;
    std::string firmware_error;
    const bool card = bus_.io().card().backup().Flush(error);
    const bool slot2 = bus_.slot2_backup().Flush(slot2_error);
    bool firmware = true;
    const uint64_t firmware_revision = bus_.io().firmware_revision();
    if (firmware_revision != firmware_flushed_revision_ && !bus_.firmware_save_path().empty()) {
        firmware = core::WriteFileReplacing(bus_.firmware_save_path(), bus_.firmware_image(), firmware_error);
        if (firmware) {
            firmware_flushed_revision_ = firmware_revision;
        }
    }
    if (card && !slot2) {
        error = slot2_error;
    } else if (card && slot2 && !firmware) {
        error = firmware_error;
    }
    return card && slot2 && firmware;
}


void Core::DumpFrameIfRequested() const {
    // Debug aid: EMUGBDS_DUMP_DS_FRAME=N writes both screens of frame N to build/debug-ds-frame.ppm.
    const char* dump_frame = std::getenv("EMUGBDS_DUMP_DS_FRAME");
    if (dump_frame == nullptr || std::strtoull(dump_frame, nullptr, 10) != frame_counter_) {
        return;
    }
    if (FILE* output = std::fopen("build/debug-ds-frame.ppm", "wb")) {
        std::fprintf(output, "P6\n%u %u\n255\n", kScreenWidth, kScreenHeight * 2u);
        for (uint32_t color : frame_) {
            const uint8_t rgb[3] = {static_cast<uint8_t>(color), static_cast<uint8_t>(color >> 8u), static_cast<uint8_t>(color >> 16u)};
            std::fwrite(rgb, sizeof(rgb), 1u, output);
        }
        std::fclose(output);
    }
}


void Core::AdvanceLcd(uint32_t from, uint32_t to) {
    Io& io = bus_.io();
    for (uint32_t line = from / kCyclesPerLine; line * kCyclesPerLine <= to; ++line) {
        const uint32_t line_start = line * kCyclesPerLine;
        if (line_start > from && line_start < kArm7CyclesPerFrame) {
            io.StartLine(static_cast<uint16_t>(line)); // Line 0 starts with the frame.
        }
        const uint32_t hblank = line_start + kHBlankStartCycle;
        if (hblank > from && hblank <= to) {
            if (line < kScreenHeight) {
                RenderLine(line); // The line is drawn by the time H-Blank starts.
            }
            io.StartHBlank();
        }
    }
}


bool Core::Reset(std::string& error) {
    bus_.ResetMemory();
    arm9_.Reset();
    arm7_.Reset();
    arm9_.set_load_interworking(true);
    arm9_.WriteCpsr(kCpsrSvc);
    arm7_.WriteCpsr(kCpsrSvc);
    arm9_halted_ = false;
    arm7_halted_ = false;
    firmware_flushed_revision_ = 0u; // ResetMemory resets the SPI revision with the rest of the I/O
    cpus_active_ = false;
    firmware_running_ = false;
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
        // Firmware boot: both CPUs start at their reset vectors and run the real BIOS and firmware,
        // whose menu starts a Slot-2 game by writing HALTCNT GBA mode.
        firmware_running_ = boot_mode_ == core::DsBootMode::DsFirmware && bus_.HasBios9() && bus_.HasBios7() &&
            bus_.HasFirmware();
        cpus_active_ = firmware_running_;
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
    // What the BIOS and firmware leave in main RAM for the game (GBATEK "NDS BIOS RAM Usage").
    const uint32_t chip_id = bus_.io().card().ChipId();
    const uint16_t header_crc = rom.size() >= 0x160u ? static_cast<uint16_t>(rom[0x15Eu] | (rom[0x15Fu] << 8u)) : 0u;
    const uint16_t secure_crc = rom.size() >= 0x6Eu ? static_cast<uint16_t>(rom[0x6Cu] | (rom[0x6Du] << 8u)) : 0u;
    for (uint32_t base : {0x027FF800u, 0x027FFC00u}) {
        bus_.arm9().Write32(base + 0x0u, chip_id);
        bus_.arm9().Write32(base + 0x4u, chip_id);
        bus_.arm9().Write16(base + 0x8u, header_crc);
        bus_.arm9().Write16(base + 0xAu, secure_crc);
    }
    bus_.arm9().Write16(0x027FF850u, 0x5835u); // ARM7 BIOS CRC
    bus_.arm9().Write16(0x027FFC10u, 0x5835u);
    bus_.arm9().Write32(0x027FF880u, 7u);      // boot handshake values at cart boot time
    bus_.arm9().Write32(0x027FF884u, 6u);
    bus_.arm9().Write16(0x027FFC40u, 1u);      // boot indicator: normal cartridge boot
    bus_.arm7().Write16(0x04000504u, 0x200u);  // SOUNDBIAS: the boot menu leaves it at the 200h midpoint
    const std::vector<uint8_t>& firmware = bus_.firmware_image();
    if (const size_t user = CurrentUserSettings(firmware)) { // current user settings copy at 27FFC80h
        for (uint32_t i = 0u; i < 0x70u; ++i) {
            bus_.arm7().Write8(0x027FFC80u + i, firmware[user + i]);
        }
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
    if (boot_mode_ != core::DsBootMode::DsFirmware || firmware_running_) {
        return false; // The real firmware makes its own choice.
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
