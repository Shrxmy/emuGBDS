#include "emugbds/ingba/Core.h"
#include "emugbds/arm/CpuConstants.h"
#include "emugbds/arm/DebugTrace.h"
#include "emugbds/ingba/MemoryConstants.h"
#include "emugbds/ingba/VideoConstants.h"

#include <algorithm>
#include <iostream>

namespace emugbds::ingba {

bool Core::Initialize(std::string& error) {
    (void)error;
    initialized_ = true;
    frame_counter_ = 0;
    last_unsupported_swi_ = 0u;
    halted_ = false;
    return Reset(error);
}


bool Core::LoadBios(const std::filesystem::path& bios_path, std::string& error) {
    return bus_.LoadBios(bios_path, error);
}


bool Core::LoadRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!FlushSave(error) || !bus_.LoadRom(rom_path, error)) { // the old game's save is written first
        return false;
    }
    rom_loaded_ = true;
    if (!initialized_) {
        if (!Initialize(error)) {
            return false;
        }
    }
    return Reset(error);
}


bool Core::RunFrame(std::string& error) {
    if (!initialized_ || !rom_loaded_) {
        error = "In-house GBA core is not initialized with a ROM";
        return false;
    }

    uint32_t cycles = 0;
    bus_.apu().ClearSamples();
    cpu_.set_trace_frame(frame_counter_);
    AdvanceLcdTiming(lcd_cycle_in_frame_);
    while (cycles < kCyclesPerFrame) {
        if (halted_) {
            const uint16_t ie = bus_.Read16(kInterruptEnableAddress);
            const uint16_t iflags = bus_.Read16(kInterruptFlagAddress);
            // HALT is released by an enabled request. IME and CPSR.I are
            // consulted separately by ServicePendingInterrupts when deciding
            // whether that request vectors into an IRQ handler.
            if (static_cast<uint16_t>(ie & iflags) != 0u) {
                halted_ = false;
            }
        }
        if (!halted_) {
#ifndef NDEBUG
            const uint32_t pre_step_pc = cpu_.state().r[15];
            const uint32_t pre_step_instr = cpu_.state().thumb
                ? static_cast<uint32_t>(bus_.Read16(pre_step_pc))
                : bus_.Read32(pre_step_pc);
            const uint32_t pre_step_cpsr = cpu_.state().cpsr;
#endif
            cpu_.state().thumb = (cpu_.state().cpsr & arm::kCpsrThumbBit) != 0u;
            ServicePendingInterrupts();
            if (!cpu_.Step(error)) {
                return false;
            }
            if (bus_.ConsumeHaltRequest()) {
                halted_ = true;
            }
#ifndef NDEBUG
            const uint32_t post_step_pc = cpu_.state().r[15];
            if (!arm::IsOpenBusPc(pre_step_pc) && arm::IsOpenBusPc(post_step_pc)) {
                arm::ReportFirstPcEscape(
                    frame_counter_,
                    pre_step_pc,
                    post_step_pc,
                    pre_step_instr,
                    pre_step_cpsr,
                    (pre_step_cpsr & arm::kCpsrThumbBit) != 0u,
                    bus_.Read32(kIrqCallbackAddress),
                    bus_.Read16(kInterruptEnableAddress),
                    bus_.Read16(kInterruptMasterEnableAddress),
                    bus_.Read16(kInterruptFlagAddress));
            }
#endif
        }
        uint32_t elapsed = 1u;
        const bool wake_pending = halted_ &&
            static_cast<uint16_t>(bus_.Read16(kInterruptEnableAddress) & bus_.Read16(kInterruptFlagAddress)) != 0u;
        if (halted_ && !wake_pending) {
            // Nothing can wake the CPU before the next LCD edge or timer overflow.
            const uint32_t line_cycle = lcd_cycle_in_frame_ % kCyclesPerScanline;
            const uint32_t to_edge = line_cycle < kHBlankStartCycle
                ? kHBlankStartCycle - line_cycle
                : kCyclesPerScanline - line_cycle;
            elapsed = std::min({to_edge, bus_.CyclesUntilTimerOverflow(), kCyclesPerFrame - cycles});
        }
        cycles += elapsed;
        bus_.AdvanceTimers(elapsed);
        lcd_cycle_in_frame_ = (lcd_cycle_in_frame_ + elapsed) % kCyclesPerFrame;
        // LCD state changes at line start and HBlank start; a new VCOUNT target is compared at once.
        const uint32_t line_cycle = lcd_cycle_in_frame_ % kCyclesPerScanline;
        if (line_cycle == 0u || line_cycle == kHBlankStartCycle || bus_.ConsumeVCountTargetWrite()) {
            AdvanceLcdTiming(lcd_cycle_in_frame_);
        }
    }

    UpdateVideoFrame();
    ++frame_counter_;
    FlushSaveWhenIdle();
    return true;
}


void Core::FlushSaveWhenIdle() {
    // Save after the game has stopped writing for a second, so bursts of writes cost one file write.
    constexpr uint32_t kIdleFrames = 60u;
    const Backup& backup = bus_.backup();
    if (backup.revision() != save_revision_) {
        save_revision_ = backup.revision();
        save_idle_frames_ = 0u;
    } else if (backup.dirty() && ++save_idle_frames_ >= kIdleFrames) {
        std::string error;
        if (!FlushSave(error)) {
            std::cerr << "[ingba] " << error << "\n";
            save_idle_frames_ = 0u; // retry after another quiet second
        }
    }
}


bool Core::FlushSave(std::string& error) {
    return bus_.backup().Flush(error);
}


bool Core::Reset(std::string& error) {
    (void)error;
    cpu_.Reset();
    cpu_.state().cpsr = 0x000000D3u;
    cpu_.state().thumb = false;
    cpu_.state().r[15] = bus_.HasBios() ? kBiosBase : kRomBase;
    last_unsupported_swi_ = 0u;
    halted_ = false;
    bus_.ConsumeHaltRequest();
    in_vblank_ = false;
    in_hblank_ = false;
    in_vcounter_match_ = false;
    lcd_cycle_in_frame_ = 0;
    bus_.ApplyKeyInputMasks(0u, 0x03FFu);
    bus_.Write16(kWaitCntAddress, 0x4317u);
    bus_.Write8(kPostFlgAddress, 0x00u);
    bus_.SetLcdStatus(0u, false, false, true);
#ifndef NDEBUG
    arm::ResetPcDebugTrace();
#endif
    video_buffer_.fill(0xFF000000u);
    return true;
}


void Core::ApplyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept {
    bus_.ApplyKeyInputMasks(press_mask, release_mask);
}


uint16_t Core::dispcnt() const noexcept {
    return bus_.Read16(kDispCntAddress);
}


uint16_t Core::dispstat() const noexcept {
    return bus_.Read16(kDispStatAddress);
}


uint16_t Core::vcount() const noexcept {
    return bus_.Read16(kVCountAddress);
}


uint16_t Core::bgcnt(uint32_t index) const noexcept {
    if (index > 3u) {
        return 0u;
    }
    return bus_.Read16(kBgCntAddressBase + (index * 2u));
}


uint16_t Core::iflags() const noexcept {
    return bus_.Read16(kInterruptFlagAddress);
}


uint16_t Core::ie() const noexcept {
    return bus_.Read16(kInterruptEnableAddress);
}


uint16_t Core::ime() const noexcept {
    return bus_.Read16(kInterruptMasterEnableAddress);
}


uint64_t Core::dispcnt_write_count() const noexcept {
    return bus_.dispcnt_write_count();
}


uint16_t Core::last_dispcnt_written() const noexcept {
    return bus_.last_dispcnt_written();
}


void Core::ServicePendingInterrupts() noexcept {
    if ((cpu_.state().cpsr & arm::kCpsrIrqDisableBit) != 0u) {
        return;
    }

    const uint16_t pending = bus_.PendingInterrupts();
    if (pending == 0u) {
        return;
    }

    halted_ = false;
    if (bus_.HasBios()) {
        const uint8_t postflg = bus_.Read8(kPostFlgAddress);
        if (postflg == 0x00u) {
            return;
        }

        // The interrupt check runs before the instruction at r15 is
        // executed. The BIOS epilogue is `SUBS PC, LR, #4`, so LR must be
        // four bytes past that next instruction in this core's address
        // representation, for both ARM and Thumb execution.
        const uint32_t return_address = cpu_.state().r[15] + 4u;
#ifndef NDEBUG
        arm::ReportFirstBiosIrqEntry(
            frame_counter_,
            cpu_.state().r[15],
            return_address,
            pending,
            bus_.Read32(kIrqCallbackAddress));
#endif
        cpu_.EnterException(arm::kCpuModeIrq, 0x00000018u, false, return_address, true);
        return;
    }

    const uint16_t bios_mirror = bus_.Read16(kBiosIrqFlagsMirrorAddress);
    bus_.Write16(kBiosIrqFlagsMirrorAddress, static_cast<uint16_t>(bios_mirror | pending));
    const uint16_t iwram_mirror = bus_.Read16(kBiosIrqFlagsMirrorIwramAddress);
    bus_.Write16(kBiosIrqFlagsMirrorIwramAddress, static_cast<uint16_t>(iwram_mirror | pending));

    const uint32_t callback = bus_.Read32(kIrqCallbackAddress);
    if (callback == 0u || callback == 0xFFFFFFFFu) {
        return;
    }
    if (callback < 0x02000000u || callback >= 0x10000000u) {
        return;
    }

    const bool callback_thumb = (callback & 1u) != 0u;
    const uint32_t handler_address = callback_thumb ? (callback & ~1u) : (callback & ~3u);
    const uint32_t return_address = (cpu_.state().r[15] + 4u) | (cpu_.state().thumb ? 1u : 0u);
    cpu_.EnterException(arm::kCpuModeIrq, handler_address, callback_thumb, return_address, true);
}


} // namespace emugbds::ingba
