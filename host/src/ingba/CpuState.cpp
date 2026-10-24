#include "emugbds/ingba/Core.h"
#include "emugbds/ingba/CpuConstants.h"
#include "emugbds/ingba/DebugTrace.h"
#include "emugbds/ingba/MemoryConstants.h"

namespace emugbds::ingba {

bool Core::HandleUnsupportedArm(UnsupportedArmClass unsupported_class, uint32_t next_pc, std::string& error) {
    RecordUnsupportedArm(unsupported_class);
    cpu_.r[15] = next_pc;
    if (unknown_arm_instruction_count_ > 10'000'000u) {
        halted_ = true;
        error = "In-house GBA core halted due to unsupported instruction volume (top class: " +
            std::string(DominantUnsupportedClassName()) +
            ", total=" + std::to_string(unknown_arm_instruction_count_) + ")";
        return false;
    }
    return true;
}


void Core::RecordUnsupportedArm(UnsupportedArmClass unsupported_class) {
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


const char* Core::DominantUnsupportedClassName() const noexcept {
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


bool Core::DecodeArmOperand2(
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


uint32_t Core::DecodeShiftedRegister(uint32_t instruction, uint32_t fetch_pc, bool& shifter_carry) const {
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


uint32_t Core::ReadArmRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    if (reg_index == 15u) {
        return fetch_pc;
    }
    return cpu_.r[reg_index & 0xFu];
}


void Core::WriteArmRegister(uint32_t reg_index, uint32_t value) {
    if (reg_index == 15u) {
        cpu_.r[15] = cpu_.thumb ? (value & ~1u) : (value & ~3u);
        return;
    }
    cpu_.r[reg_index & 0xFu] = value;
}


uint32_t Core::ReadArmUserRegister(uint32_t reg_index, uint32_t fetch_pc) const {
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


void Core::WriteArmUserRegister(uint32_t reg_index, uint32_t value) {
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


uint32_t Core::ReadThumbRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    if (reg_index == 15u) {
        return fetch_pc;
    }
    return cpu_.r[reg_index & 0xFu];
}


void Core::WriteThumbRegister(uint32_t reg_index, uint32_t value) {
    if (reg_index == 15u) {
        cpu_.r[15] = value & ~1u;
        return;
    }
    cpu_.r[reg_index & 0xFu] = value;
}

void Core::SetThumbState(bool enabled) noexcept {
    cpu_.thumb = enabled;
    if (enabled) {
        cpu_.cpsr |= kCpsrThumbBit;
    } else {
        cpu_.cpsr &= ~kCpsrThumbBit;
    }
}


void Core::SetNFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | (value ? (1u << 31) : 0u);
}


void Core::SetZFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | (value ? (1u << 30) : 0u);
}


void Core::SetCFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (value ? (1u << 29) : 0u);
}


void Core::SetVFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 28)) | (value ? (1u << 28) : 0u);
}


void Core::SetNZFlags(uint32_t value) {
    SetNFlag((value & 0x80000000u) != 0u);
    SetZFlag(value == 0u);
}


bool Core::ConditionPassed(uint32_t instruction) const {
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


uint32_t Core::SignExtend24(uint32_t value) {
    if ((value & 0x00800000u) != 0) {
        return value | 0xFF000000u;
    }
    return value;
}


int32_t Core::SignExtend(uint32_t value, uint32_t bits) {
    const uint32_t shift = 32u - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}


bool Core::IsValidCpuMode(uint32_t mode) noexcept {
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


bool Core::ModeHasSpsr(uint32_t mode) noexcept {
    return mode == kCpuModeFiq ||
        mode == kCpuModeIrq ||
        mode == kCpuModeSupervisor ||
        mode == kCpuModeAbort ||
        mode == kCpuModeUndefined;
}


uint32_t Core::CurrentMode() const noexcept {
    return cpu_.cpsr & kCpuModeMask;
}


bool Core::CurrentModeIsPrivileged() const noexcept {
    return CurrentMode() != kCpuModeUser;
}


void Core::SaveBankedRegistersForMode(uint32_t mode) noexcept {
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


void Core::RestoreBankedRegistersForMode(uint32_t mode) noexcept {
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


void Core::SwitchCpuMode(uint32_t new_mode) noexcept {
    const uint32_t current_mode = CurrentMode();
    if (new_mode == current_mode || !IsValidCpuMode(new_mode)) {
        return;
    }

    SaveBankedRegistersForMode(current_mode);
    cpu_.cpsr = (cpu_.cpsr & ~kCpuModeMask) | new_mode;
    RestoreBankedRegistersForMode(new_mode);
}


uint32_t Core::ReadSpsrForMode(uint32_t mode) const noexcept {
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


void Core::WriteSpsrForMode(uint32_t mode, uint32_t value) noexcept {
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


uint32_t Core::ReadCurrentSpsr() const noexcept {
    return ReadSpsrForMode(CurrentMode());
}


bool Core::WriteCurrentSpsr(uint32_t value) noexcept {
    const uint32_t mode = CurrentMode();
    if (!ModeHasSpsr(mode)) {
        return false;
    }
    WriteSpsrForMode(mode, value);
    return true;
}


void Core::WriteCpsr(uint32_t value) noexcept {
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


void Core::EnterException(
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
    RecordPcFlowEvent(site, old_pc, cpu_.r[15], 0u, cpu_.cpsr, cpu_.r[14], cpu_.r[13], frame_counter_);
#endif
}


void Core::ServicePendingInterrupts() noexcept {
    if ((cpu_.cpsr & kCpsrIrqDisableBit) != 0u) {
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
        const uint32_t return_address = cpu_.r[15] + 4u;
#ifndef NDEBUG
        ReportFirstBiosIrqEntry(
            frame_counter_,
            cpu_.r[15],
            return_address,
            pending,
            bus_.Read32(kIrqCallbackAddress));
#endif
        EnterException(kCpuModeIrq, 0x00000018u, false, return_address, true);
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
    const uint32_t return_address = (cpu_.r[15] + 4u) | (cpu_.thumb ? 1u : 0u);
    EnterException(kCpuModeIrq, handler_address, callback_thumb, return_address, true);
}


uint32_t Core::Ror(uint32_t value, uint32_t amount) {
    const uint32_t rotated = amount & 31u;
    if (rotated == 0u) {
        return value;
    }
    return (value >> rotated) | (value << (32u - rotated));
}


Core::AddResult Core::AddWithCarry(uint32_t lhs, uint32_t rhs, bool carry_in) {
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


} // namespace emugbds::ingba
