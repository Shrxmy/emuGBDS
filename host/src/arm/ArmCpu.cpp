#include "emugbds/arm/ArmCpu.h"
#include "emugbds/arm/CpuConstants.h"
#include "emugbds/arm/DebugTrace.h"

namespace emugbds::arm {

void ArmCpu::Reset() noexcept {
    cpu_ = {};
    unknown_arm_instruction_count_ = 0;
    unsupported_arm_stats_ = {};
}


bool ArmCpu::Step(std::string& error) {
    const uint32_t pc = cpu_.r[15];
    const uint32_t width = (cpu_.cpsr & kCpsrThumbBit) != 0u ? 2u : 4u;
    const bool ok = ExecuteNext(error);
    if (cpu_.r[15] != pc + width) {
        // Jumps (branches, PC loads, exceptions) refill the pipeline: B/BL/SWI are 2S+1N.
        bus_.Refill(cpu_.r[15], (cpu_.cpsr & kCpsrThumbBit) != 0u ? 2u : 4u);
    }
    return ok;
}


bool ArmCpu::ExecuteNext(std::string& error) {
    cpu_.thumb = (cpu_.cpsr & kCpsrThumbBit) != 0u;

    if (cpu_.thumb) {
        const uint16_t instruction = bus_.Fetch16(cpu_.r[15]);
        const uint32_t pc = cpu_.r[15];
        const uint32_t fetch_pc = pc + 4u;
        return ExecuteThumbInstruction(instruction, pc, fetch_pc, error);
    }

    const uint32_t pc = cpu_.r[15];
    const uint32_t fetch_pc = pc + 8;
    const uint32_t instruction = bus_.Fetch32(pc);
    const uint32_t next_pc = pc + 4;

    if (v5_ && (instruction >> 28u) == 0xFu) {
        return ExecuteArmUnconditional(instruction, fetch_pc, next_pc, error); // ARMv4 treats cond=1111 as never.
    }
    if ((instruction >> 28u) != 0xEu && !ConditionPassed(instruction)) {
        cpu_.r[15] = next_pc;
        return true;
    }
    // Every ARMv5 addition decoded there sits in the miscellaneous-instruction space or is MRC/MCR.
    if (v5_ && ((instruction & 0x0F900000u) == 0x01000000u || (instruction & 0x0F000010u) == 0x0E000010u)) {
        bool handled = false;
        const bool ok = ExecuteArmV5Instruction(instruction, fetch_pc, next_pc, handled);
        if (handled) {
            return ok || HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
        }
    }

    if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
        const uint32_t rn = instruction & 0xFu;
        const uint32_t target = ReadArmRegister(rn, fetch_pc);
        const bool next_thumb = (target & 1u) != 0u;
        SetThumbState(next_thumb);
        cpu_.r[15] = next_thumb ? (target & ~1u) : (target & ~3u);
#ifndef NDEBUG
        RecordPcFlowEvent("ARM.BX", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
#endif
        return true;
    }

    if ((instruction & 0x0E000000u) == 0x0A000000u) {
        const int32_t signed_offset = static_cast<int32_t>(SignExtend24(instruction & 0x00FFFFFFu) << 2);
        if ((instruction & (1u << 24)) != 0u) {
            cpu_.r[14] = next_pc;
        }
        cpu_.r[15] = fetch_pc + static_cast<uint32_t>(signed_offset);
#ifndef NDEBUG
        RecordPcFlowEvent("ARM.B/BL", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
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
        if ((instruction & 0x0FB00FF0u) == 0x01000090u) {
            return ExecuteArmSwap(instruction, next_pc);
        }
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


bool ArmCpu::HandleUnsupportedArm(UnsupportedArmClass unsupported_class, uint32_t next_pc, std::string& error) {
    RecordUnsupportedArm(unsupported_class);
    cpu_.r[15] = next_pc;
    if (unknown_arm_instruction_count_ > 10'000'000u) {
        error = "In-house GBA core halted due to unsupported instruction volume (top class: " +
            std::string(DominantUnsupportedClassName()) +
            ", total=" + std::to_string(unknown_arm_instruction_count_) + ")";
        return false;
    }
    return true;
}


void ArmCpu::RecordUnsupportedArm(UnsupportedArmClass unsupported_class) {
    ++unknown_arm_instruction_count_;
    switch (unsupported_class) {
    case UnsupportedArmClass::Generic:
        ++unsupported_arm_stats_.generic;
        break;
    case UnsupportedArmClass::Thumb:
        ++unsupported_arm_stats_.thumb;
        break;
    case UnsupportedArmClass::Multiply:
        ++unsupported_arm_stats_.multiply;
        break;
    case UnsupportedArmClass::BlockTransfer:
        ++unsupported_arm_stats_.block_transfer;
        break;
    case UnsupportedArmClass::SoftwareInterrupt:
        ++unsupported_arm_stats_.software_interrupt;
        break;
    }
}


const char* ArmCpu::DominantUnsupportedClassName() const noexcept {
    const uint64_t generic = unsupported_arm_stats_.generic;
    const uint64_t thumb = unsupported_arm_stats_.thumb;
    const uint64_t multiply = unsupported_arm_stats_.multiply;
    const uint64_t block_transfer = unsupported_arm_stats_.block_transfer;
    const uint64_t software_interrupt = unsupported_arm_stats_.software_interrupt;

    uint64_t best = generic;
    const char* name = "generic";
    if (thumb > best) {
        best = thumb;
        name = "thumb";
    }
    if (multiply > best) {
        best = multiply;
        name = "multiply";
    }
    if (block_transfer > best) {
        best = block_transfer;
        name = "block-transfer";
    }
    if (software_interrupt > best) {
        name = "software-interrupt";
    }
    return name;
}


bool ArmCpu::DecodeArmOperand2(
    uint32_t instruction, uint32_t fetch_pc, uint32_t& operand2, bool& shifter_carry) const {
    if ((instruction & (1u << 25)) != 0u) {
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t rotate = ((instruction >> 8) & 0xFu) * 2u;
        operand2 = Ror(imm8, rotate);
        if (rotate != 0u) {
            shifter_carry = (operand2 & 0x80000000u) != 0;
        }
        return true;
    }

    operand2 = DecodeShiftedRegister(instruction, fetch_pc, shifter_carry);
    return true;
}


uint32_t ArmCpu::DecodeShiftedRegister(uint32_t instruction, uint32_t fetch_pc, bool& shifter_carry) const {
    const uint32_t rm = instruction & 0xFu;
    const uint32_t shift_type = (instruction >> 5) & 0x3u;
    const bool shift_by_register = (instruction & (1u << 4)) != 0u;

    uint32_t shift_amount = 0;
    if (shift_by_register) {
        const uint32_t rs = (instruction >> 8) & 0xFu;
        shift_amount = ReadArmRegister(rs, fetch_pc) & 0xFFu;
    } else {
        shift_amount = (instruction >> 7) & 0x1Fu;
    }

    const uint32_t value = ReadArmRegister(rm, fetch_pc);
    const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;

    switch (shift_type) {
    case 0x0: // LSL
        if (shift_amount == 0) {
            shifter_carry = carry_in;
            return value;
        }
        if (shift_amount < 32u) {
            shifter_carry = ((value >> (32u - shift_amount)) & 1u) != 0u;
            return value << shift_amount;
        }
        if (shift_amount == 32u) {
            shifter_carry = (value & 1u) != 0u;
            return 0u;
        }
        shifter_carry = false;
        return 0u;
    case 0x1: { // LSR
        uint32_t amount = shift_amount;
        if (!shift_by_register && amount == 0) {
            amount = 32;
        }
        if (amount == 0) {
            shifter_carry = carry_in;
            return value;
        }
        if (amount < 32u) {
            shifter_carry = ((value >> (amount - 1u)) & 1u) != 0u;
            return value >> amount;
        }
        if (amount == 32u) {
            shifter_carry = ((value >> 31u) & 1u) != 0u;
            return 0u;
        }
        shifter_carry = false;
        return 0u;
    }
    case 0x2: { // ASR
        uint32_t amount = shift_amount;
        if (!shift_by_register && amount == 0) {
            amount = 32;
        }
        if (amount == 0) {
            shifter_carry = carry_in;
            return value;
        }
        if (amount < 32u) {
            shifter_carry = ((value >> (amount - 1u)) & 1u) != 0u;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
        }
        const bool sign = (value & 0x80000000u) != 0u;
        shifter_carry = sign;
        return sign ? 0xFFFFFFFFu : 0u;
    }
    case 0x3: { // ROR / RRX
        if (shift_amount == 0) {
            if (!shift_by_register) {
                shifter_carry = (value & 1u) != 0u;
                return (carry_in ? 0x80000000u : 0u) | (value >> 1);
            }
            shifter_carry = carry_in;
            return value;
        }

        const uint32_t amount = shift_amount & 31u;
        if (amount == 0u) {
            shifter_carry = ((value >> 31u) & 1u) != 0u;
            return value;
        }
        const uint32_t rotated = Ror(value, amount);
        shifter_carry = ((rotated >> 31u) & 1u) != 0u;
        return rotated;
    }
    default:
        shifter_carry = carry_in;
        return value;
    }
}


uint32_t ArmCpu::ReadArmRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    if (reg_index == 15u) {
        return fetch_pc;
    }
    return cpu_.r[reg_index & 0xFu];
}


void ArmCpu::WriteArmRegister(uint32_t reg_index, uint32_t value) {
    if (reg_index == 15u) {
        cpu_.r[15] = cpu_.thumb ? (value & ~1u) : (value & ~3u);
        return;
    }
    cpu_.r[reg_index & 0xFu] = value;
}


uint32_t ArmCpu::ReadArmUserRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    reg_index &= 0xFu;
    if (reg_index == 15u) {
        return fetch_pc;
    }
    if (reg_index >= 8u && reg_index <= 12u) {
        return CurrentMode() == kCpuModeFiq ? cpu_.shared_r8_r12[reg_index - 8u] : cpu_.r[reg_index];
    }
    if (reg_index == 13u || reg_index == 14u) {
        if (CurrentMode() != kCpuModeUser && CurrentMode() != kCpuModeSystem) {
            return cpu_.banked_r13_r14_usr[reg_index - 13u];
        }
    }
    return cpu_.r[reg_index];
}


void ArmCpu::WriteArmUserRegister(uint32_t reg_index, uint32_t value) {
    reg_index &= 0xFu;
    if (reg_index == 15u) {
        WriteArmRegister(reg_index, value);
        return;
    }
    if (reg_index >= 8u && reg_index <= 12u) {
        if (CurrentMode() == kCpuModeFiq) {
            cpu_.shared_r8_r12[reg_index - 8u] = value;
        } else {
            cpu_.r[reg_index] = value;
        }
        return;
    }
    if ((reg_index == 13u || reg_index == 14u) &&
        CurrentMode() != kCpuModeUser && CurrentMode() != kCpuModeSystem) {
        cpu_.banked_r13_r14_usr[reg_index - 13u] = value;
        return;
    }
    cpu_.r[reg_index] = value;
}


uint32_t ArmCpu::ReadThumbRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    if (reg_index == 15u) {
        return fetch_pc;
    }
    return cpu_.r[reg_index & 0xFu];
}


void ArmCpu::WriteThumbRegister(uint32_t reg_index, uint32_t value) {
    if (reg_index == 15u) {
        cpu_.r[15] = value & ~1u;
        return;
    }
    cpu_.r[reg_index & 0xFu] = value;
}

void ArmCpu::SetThumbState(bool enabled) noexcept {
    cpu_.thumb = enabled;
    if (enabled) {
        cpu_.cpsr |= kCpsrThumbBit;
    } else {
        cpu_.cpsr &= ~kCpsrThumbBit;
    }
}


void ArmCpu::SetNFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | (value ? (1u << 31) : 0u);
}


void ArmCpu::SetZFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | (value ? (1u << 30) : 0u);
}


void ArmCpu::SetCFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (value ? (1u << 29) : 0u);
}


void ArmCpu::SetVFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 28)) | (value ? (1u << 28) : 0u);
}


void ArmCpu::SetNZFlags(uint32_t value) {
    SetNFlag((value & 0x80000000u) != 0u);
    SetZFlag(value == 0u);
}


bool ArmCpu::ConditionPassed(uint32_t instruction) const {
    const uint32_t cond = (instruction >> 28) & 0xFu;
    const bool n = (cpu_.cpsr & (1u << 31)) != 0;
    const bool z = (cpu_.cpsr & (1u << 30)) != 0;
    const bool c = (cpu_.cpsr & (1u << 29)) != 0;
    const bool v = (cpu_.cpsr & (1u << 28)) != 0;

    switch (cond) {
    case 0x0:
        return z;
    case 0x1:
        return !z;
    case 0x2:
        return c;
    case 0x3:
        return !c;
    case 0x4:
        return n;
    case 0x5:
        return !n;
    case 0x6:
        return v;
    case 0x7:
        return !v;
    case 0x8:
        return c && !z;
    case 0x9:
        return !c || z;
    case 0xA:
        return n == v;
    case 0xB:
        return n != v;
    case 0xC:
        return !z && (n == v);
    case 0xD:
        return z || (n != v);
    case 0xE:
        return true;
    default:
        return false;
    }
}


uint32_t ArmCpu::SignExtend24(uint32_t value) {
    if ((value & 0x00800000u) != 0) {
        return value | 0xFF000000u;
    }
    return value;
}


int32_t ArmCpu::SignExtend(uint32_t value, uint32_t bits) {
    const uint32_t shift = 32u - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}


bool ArmCpu::IsValidCpuMode(uint32_t mode) noexcept {
    switch (mode) {
    case kCpuModeUser:
    case kCpuModeFiq:
    case kCpuModeIrq:
    case kCpuModeSupervisor:
    case kCpuModeAbort:
    case kCpuModeUndefined:
    case kCpuModeSystem:
        return true;
    default:
        return false;
    }
}


bool ArmCpu::ModeHasSpsr(uint32_t mode) noexcept {
    return mode == kCpuModeFiq ||
        mode == kCpuModeIrq ||
        mode == kCpuModeSupervisor ||
        mode == kCpuModeAbort ||
        mode == kCpuModeUndefined;
}


uint32_t ArmCpu::CurrentMode() const noexcept {
    return cpu_.cpsr & kCpuModeMask;
}


bool ArmCpu::CurrentModeIsPrivileged() const noexcept {
    return CurrentMode() != kCpuModeUser;
}


void ArmCpu::SaveBankedRegistersForMode(uint32_t mode) noexcept {
    if (mode != kCpuModeFiq && IsValidCpuMode(mode)) {
        for (uint32_t reg = 8u; reg <= 12u; ++reg) {
            cpu_.shared_r8_r12[reg - 8u] = cpu_.r[reg];
        }
    }

    switch (mode) {
    case kCpuModeUser:
    case kCpuModeSystem:
        cpu_.banked_r13_r14_usr[0] = cpu_.r[13];
        cpu_.banked_r13_r14_usr[1] = cpu_.r[14];
        break;
    case kCpuModeFiq:
        for (uint32_t reg = 8u; reg <= 14u; ++reg) {
            cpu_.banked_r8_r14_fiq[reg - 8u] = cpu_.r[reg];
        }
        break;
    case kCpuModeIrq:
        cpu_.banked_r13_r14_irq[0] = cpu_.r[13];
        cpu_.banked_r13_r14_irq[1] = cpu_.r[14];
        break;
    case kCpuModeSupervisor:
        cpu_.banked_r13_r14_svc[0] = cpu_.r[13];
        cpu_.banked_r13_r14_svc[1] = cpu_.r[14];
        break;
    case kCpuModeAbort:
        cpu_.banked_r13_r14_abt[0] = cpu_.r[13];
        cpu_.banked_r13_r14_abt[1] = cpu_.r[14];
        break;
    case kCpuModeUndefined:
        cpu_.banked_r13_r14_und[0] = cpu_.r[13];
        cpu_.banked_r13_r14_und[1] = cpu_.r[14];
        break;
    default:
        break;
    }
}


void ArmCpu::RestoreBankedRegistersForMode(uint32_t mode) noexcept {
    if (mode != kCpuModeFiq && IsValidCpuMode(mode)) {
        for (uint32_t reg = 8u; reg <= 12u; ++reg) {
            cpu_.r[reg] = cpu_.shared_r8_r12[reg - 8u];
        }
    }

    switch (mode) {
    case kCpuModeUser:
    case kCpuModeSystem:
        cpu_.r[13] = cpu_.banked_r13_r14_usr[0];
        cpu_.r[14] = cpu_.banked_r13_r14_usr[1];
        break;
    case kCpuModeFiq:
        for (uint32_t reg = 8u; reg <= 14u; ++reg) {
            cpu_.r[reg] = cpu_.banked_r8_r14_fiq[reg - 8u];
        }
        break;
    case kCpuModeIrq:
        cpu_.r[13] = cpu_.banked_r13_r14_irq[0];
        cpu_.r[14] = cpu_.banked_r13_r14_irq[1];
        break;
    case kCpuModeSupervisor:
        cpu_.r[13] = cpu_.banked_r13_r14_svc[0];
        cpu_.r[14] = cpu_.banked_r13_r14_svc[1];
        break;
    case kCpuModeAbort:
        cpu_.r[13] = cpu_.banked_r13_r14_abt[0];
        cpu_.r[14] = cpu_.banked_r13_r14_abt[1];
        break;
    case kCpuModeUndefined:
        cpu_.r[13] = cpu_.banked_r13_r14_und[0];
        cpu_.r[14] = cpu_.banked_r13_r14_und[1];
        break;
    default:
        break;
    }
}


void ArmCpu::SwitchCpuMode(uint32_t new_mode) noexcept {
    const uint32_t current_mode = CurrentMode();
    if (new_mode == current_mode || !IsValidCpuMode(new_mode)) {
        return;
    }

    SaveBankedRegistersForMode(current_mode);
    cpu_.cpsr = (cpu_.cpsr & ~kCpuModeMask) | new_mode;
    RestoreBankedRegistersForMode(new_mode);
}


uint32_t ArmCpu::ReadSpsrForMode(uint32_t mode) const noexcept {
    switch (mode) {
    case kCpuModeFiq:
        return cpu_.spsr_fiq;
    case kCpuModeIrq:
        return cpu_.spsr_irq;
    case kCpuModeSupervisor:
        return cpu_.spsr_svc;
    case kCpuModeAbort:
        return cpu_.spsr_abt;
    case kCpuModeUndefined:
        return cpu_.spsr_und;
    default:
        return 0u;
    }
}


void ArmCpu::WriteSpsrForMode(uint32_t mode, uint32_t value) noexcept {
    switch (mode) {
    case kCpuModeFiq:
        cpu_.spsr_fiq = value;
        break;
    case kCpuModeIrq:
        cpu_.spsr_irq = value;
        break;
    case kCpuModeSupervisor:
        cpu_.spsr_svc = value;
        break;
    case kCpuModeAbort:
        cpu_.spsr_abt = value;
        break;
    case kCpuModeUndefined:
        cpu_.spsr_und = value;
        break;
    default:
        break;
    }
}


uint32_t ArmCpu::ReadCurrentSpsr() const noexcept {
    return ReadSpsrForMode(CurrentMode());
}


bool ArmCpu::WriteCurrentSpsr(uint32_t value) noexcept {
    const uint32_t mode = CurrentMode();
    if (!ModeHasSpsr(mode)) {
        return false;
    }
    WriteSpsrForMode(mode, value);
    return true;
}


void ArmCpu::WriteCpsr(uint32_t value) noexcept {
    const uint32_t current_mode = CurrentMode();
    uint32_t next_mode = value & kCpuModeMask;
    if (!IsValidCpuMode(next_mode)) {
        next_mode = current_mode;
        value = (value & ~kCpuModeMask) | next_mode;
    }

    if (next_mode != current_mode) {
        SaveBankedRegistersForMode(current_mode);
        RestoreBankedRegistersForMode(next_mode);
    }

    cpu_.cpsr = value;
    cpu_.thumb = (cpu_.cpsr & kCpsrThumbBit) != 0u;
}


void ArmCpu::EnterException(
    uint32_t target_mode,
    uint32_t handler_address,
    bool handler_thumb,
    uint32_t return_address,
    bool mask_irq) noexcept {
    const uint32_t saved_cpsr = cpu_.cpsr;
    WriteSpsrForMode(target_mode, saved_cpsr);

    uint32_t exception_cpsr = (saved_cpsr & ~kCpuModeMask) | target_mode;
    if (mask_irq) {
        exception_cpsr |= kCpsrIrqDisableBit;
    }
    if (handler_thumb) {
        exception_cpsr |= kCpsrThumbBit;
    } else {
        exception_cpsr &= ~kCpsrThumbBit;
    }

    WriteCpsr(exception_cpsr);
    cpu_.r[14] = return_address;
    const uint32_t old_pc = cpu_.r[15];
    cpu_.r[15] = handler_thumb ? (handler_address & ~1u) : (handler_address & ~3u);
#ifndef NDEBUG
    const char* site = "EXC";
    if (target_mode == kCpuModeIrq) {
        site = "EXC.IRQ";
    } else if (target_mode == kCpuModeSupervisor) {
        site = "EXC.SVC";
    }
    RecordPcFlowEvent(site, old_pc, cpu_.r[15], 0u, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
#endif
}


uint32_t ArmCpu::Ror(uint32_t value, uint32_t amount) {
    const uint32_t rotated = amount & 31u;
    if (rotated == 0u) {
        return value;
    }
    return (value >> rotated) | (value << (32u - rotated));
}


ArmCpu::AddResult ArmCpu::AddWithCarry(uint32_t lhs, uint32_t rhs, bool carry_in) {
    const uint64_t carry = carry_in ? 1ull : 0ull;
    const uint64_t wide = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs) + carry;
    AddResult out;
    out.value = static_cast<uint32_t>(wide);
    out.carry = (wide >> 32u) != 0u;

    const bool lhs_sign = (lhs & 0x80000000u) != 0u;
    const bool rhs_sign = (rhs & 0x80000000u) != 0u;
    const bool out_sign = (out.value & 0x80000000u) != 0u;
    out.overflow = (lhs_sign == rhs_sign) && (lhs_sign != out_sign);
    return out;
}


} // namespace emugbds::arm
