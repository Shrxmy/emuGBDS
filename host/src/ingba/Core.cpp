#include "emugbds/ingba/Core.h"
#include "emugbds/ingba/CpuConstants.h"
#include "emugbds/ingba/DebugTrace.h"
#include "emugbds/ingba/MemoryConstants.h"
#include "emugbds/ingba/VideoConstants.h"

#include <algorithm>

namespace emugbds::ingba {

bool Core::Initialize(std::string& error) {
    (void)error;
    initialized_ = true;
    frame_counter_ = 0;
    unknown_arm_instruction_count_ = 0;
    unsupported_arm_stats_ = {};
    last_unsupported_swi_ = 0u;
    halted_ = false;
    return Reset(error);
}


bool Core::LoadBios(const std::filesystem::path& bios_path, std::string& error) {
    return bus_.LoadBios(bios_path, error);
}


bool Core::LoadRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!bus_.LoadRom(rom_path, error)) {
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
            const uint32_t pre_step_pc = cpu_.r[15];
            const uint32_t pre_step_instr = cpu_.thumb
                ? static_cast<uint32_t>(bus_.Read16(pre_step_pc))
                : bus_.Read32(pre_step_pc);
            const uint32_t pre_step_cpsr = cpu_.cpsr;
#endif
            if (!StepArm(error)) {
                return false;
            }
            if (bus_.ConsumeHaltRequest()) {
                halted_ = true;
            }
#ifndef NDEBUG
            const uint32_t post_step_pc = cpu_.r[15];
            if (!IsOpenBusPc(pre_step_pc) && IsOpenBusPc(post_step_pc)) {
                ReportFirstPcEscape(
                    frame_counter_,
                    pre_step_pc,
                    post_step_pc,
                    pre_step_instr,
                    pre_step_cpsr,
                    (pre_step_cpsr & kCpsrThumbBit) != 0u,
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
    return true;
}


bool Core::Reset(std::string& error) {
    (void)error;
    cpu_ = {};
    cpu_.cpsr = 0x000000D3u;
    cpu_.thumb = false;
    cpu_.r[15] = bus_.HasBios() ? kBiosBase : kRomBase;
    unknown_arm_instruction_count_ = 0;
    unsupported_arm_stats_ = {};
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
    ResetPcDebugTrace();
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


bool Core::StepArm(std::string& error) {
    cpu_.thumb = (cpu_.cpsr & kCpsrThumbBit) != 0u;
    ServicePendingInterrupts();
    cpu_.thumb = (cpu_.cpsr & kCpsrThumbBit) != 0u;

    if (cpu_.thumb) {
        const uint16_t instruction = bus_.Read16(cpu_.r[15]);
        const uint32_t pc = cpu_.r[15];
        const uint32_t fetch_pc = pc + 4u;
        return ExecuteThumbInstruction(instruction, pc, fetch_pc, error);
    }

    const uint32_t pc = cpu_.r[15];
    const uint32_t fetch_pc = pc + 8;
    const uint32_t instruction = bus_.Read32(pc);
    const uint32_t next_pc = pc + 4;

    if (!ConditionPassed(instruction)) {
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
        const uint32_t rn = instruction & 0xFu;
        const uint32_t target = ReadArmRegister(rn, fetch_pc);
        const bool next_thumb = (target & 1u) != 0u;
        SetThumbState(next_thumb);
        cpu_.r[15] = next_thumb ? (target & ~1u) : (target & ~3u);
#ifndef NDEBUG
        RecordPcFlowEvent("ARM.BX", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], frame_counter_);
#endif
        return true;
    }

    if ((instruction & 0x0E000000u) == 0x0A000000u) {
        const int32_t signed_offset = static_cast<int32_t>(SignExtend24(instruction & 0x00FFFFFFu) << 2);
        if ((instruction & (1u << 24)) != 0u) {
            cpu_.r[14] = next_pc;
        }
        cpu_.r[15] = static_cast<uint32_t>(static_cast<int32_t>(fetch_pc) + signed_offset);
#ifndef NDEBUG
        RecordPcFlowEvent("ARM.B/BL", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], frame_counter_);
#endif
        return true;
    }

    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        if (ExecuteArmSoftwareInterrupt(instruction, next_pc, error)) {
            return true;
        }
        return HandleUnsupportedArm(UnsupportedArmClass::SoftwareInterrupt, next_pc, error);
    }

    const uint32_t top2 = (instruction >> 26) & 0x3u;
    if (top2 == 0x0u) {
        const bool maybe_msr_imm =
            ((instruction & 0x0FB00000u) == 0x03200000u) &&
            (((instruction >> 12u) & 0xFu) == 0xFu);
        if ((instruction & 0x0F900FF0u) == 0x01000000u || // MRS
            (instruction & 0x0DB0F000u) == 0x0120F000u || // MSR register
            maybe_msr_imm) { // MSR immediate
            if (ExecuteArmPsrTransfer(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
        }
        if ((instruction & 0x0E000090u) == 0x00000090u && (instruction & 0x00000060u) != 0u) {
            if (ExecuteArmHalfwordTransfer(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
        }
        if ((instruction & 0x0F8000F0u) == 0x00800090u) {
            if (ExecuteArmMultiplyLong(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Multiply, next_pc, error);
        }
        if ((instruction & 0x0FC000F0u) == 0x00000090u) {
            if (ExecuteArmMultiply(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Multiply, next_pc, error);
        } else if (ExecuteArmDataProcessing(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
    } else if (top2 == 0x1u) {
        if (ExecuteArmSingleDataTransfer(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
    } else if ((instruction & 0x0E000000u) == 0x08000000u) {
        if (ExecuteArmBlockDataTransfer(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
        return HandleUnsupportedArm(UnsupportedArmClass::BlockTransfer, next_pc, error);
    }

    if (instruction == 0xE1A00000u || instruction == 0xE320F000u) {
        // NOP forms: MOV r0, r0 and YIELD as no-op.
        cpu_.r[15] = next_pc;
        return true;
    }

    return HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
}


} // namespace emugbds::ingba
