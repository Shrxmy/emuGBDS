#include "emugbds/arm/ArmCpu.h"
#include "emugbds/arm/CpuConstants.h"
#include "emugbds/arm/DebugTrace.h"

namespace emugbds::arm {

bool ArmCpu::ExecuteArmDataProcessing(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const uint32_t opcode = (instruction >> 21) & 0xFu;
    const bool set_flags = (instruction & (1u << 20)) != 0;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t rd = (instruction >> 12) & 0xFu;

    uint32_t operand2 = 0;
    bool shifter_carry = (cpu_.cpsr & (1u << 29)) != 0;
    if (!DecodeArmOperand2(instruction, fetch_pc, operand2, shifter_carry)) {
        return false;
    }

    const uint32_t operand1 = ReadArmRegister(rn, fetch_pc);

    uint32_t result = 0;
    bool write_result = true;
    bool use_logic_flags = false;
    bool carry_out = false;
    bool overflow_out = false;

    switch (opcode) {
    case 0x0: // AND
        result = operand1 & operand2;
        use_logic_flags = true;
        break;
    case 0x1: // EOR
        result = operand1 ^ operand2;
        use_logic_flags = true;
        break;
    case 0x2: { // SUB
        const AddResult add = AddWithCarry(operand1, ~operand2, true);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x3: { // RSB
        const AddResult add = AddWithCarry(operand2, ~operand1, true);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x4: { // ADD
        const AddResult add = AddWithCarry(operand1, operand2, false);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x5: { // ADC
        const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;
        const AddResult add = AddWithCarry(operand1, operand2, carry_in);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x6: { // SBC
        const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;
        const AddResult add = AddWithCarry(operand1, ~operand2, carry_in);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x7: { // RSC
        const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;
        const AddResult add = AddWithCarry(operand2, ~operand1, carry_in);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x8: // TST
        result = operand1 & operand2;
        write_result = false;
        use_logic_flags = true;
        break;
    case 0x9: // TEQ
        result = operand1 ^ operand2;
        write_result = false;
        use_logic_flags = true;
        break;
    case 0xA: { // CMP
        const AddResult add = AddWithCarry(operand1, ~operand2, true);
        result = add.value;
        write_result = false;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0xB: { // CMN
        const AddResult add = AddWithCarry(operand1, operand2, false);
        result = add.value;
        write_result = false;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0xC: // ORR
        result = operand1 | operand2;
        use_logic_flags = true;
        break;
    case 0xD: // MOV
        result = operand2;
        use_logic_flags = true;
        break;
    case 0xE: // BIC
        result = operand1 & ~operand2;
        use_logic_flags = true;
        break;
    case 0xF: // MVN
        result = ~operand2;
        use_logic_flags = true;
        break;
    default:
        return false;
    }

    if (set_flags) {
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | ((result & 0x80000000u) ? (1u << 31) : 0u);
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | ((result == 0) ? (1u << 30) : 0u);

        if (use_logic_flags) {
            cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (shifter_carry ? (1u << 29) : 0u);
        } else {
            cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (carry_out ? (1u << 29) : 0u);
            cpu_.cpsr = (cpu_.cpsr & ~(1u << 28)) | (overflow_out ? (1u << 28) : 0u);
        }
    }

    if (write_result) {
        const uint32_t old_pc = cpu_.r[15];
        if (rd == 15u && set_flags && ModeHasSpsr(CurrentMode())) {
            const uint32_t mode_before_restore = CurrentMode();
            const uint32_t spsr = ReadCurrentSpsr();
            const uint32_t exception_lr = cpu_.r[14];
            const uint32_t restored_pc = (spsr & kCpsrThumbBit) != 0u
                ? (result & ~1u)
                : (result & ~3u);
            WriteCpsr(spsr);
            cpu_.r[15] = restored_pc;
            RecordPcFlowEvent("ARM.DP.PC", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
#ifndef NDEBUG
            if (mode_before_restore == kCpuModeIrq) {
                ReportFirstIrqReturn(trace_frame_, old_pc, instruction, cpu_.r[15], spsr, exception_lr);
            }
#endif
            return true;
        }

        WriteArmRegister(rd, result);
        if (rd == 15u) {
            RecordPcFlowEvent("ARM.DP.PC", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
            return true;
        }
    }

    cpu_.r[15] = next_pc;
    return true;
}


bool ArmCpu::ExecuteArmPsrTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool is_mrs = (instruction & 0x0FBF0FFFu) == 0x010F0000u;
    if (is_mrs) {
        const bool use_spsr = (instruction & (1u << 22)) != 0u;
        const uint32_t rd = (instruction >> 12u) & 0xFu;
        if (use_spsr) {
            if (!ModeHasSpsr(CurrentMode())) {
                WriteArmRegister(rd, cpu_.cpsr);
                cpu_.r[15] = next_pc;
                return true;
            }
            WriteArmRegister(rd, ReadCurrentSpsr());
        } else {
            WriteArmRegister(rd, cpu_.cpsr);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    const bool is_msr_reg = (instruction & 0x0DB0F010u) == 0x0120F000u;
    const bool is_msr_imm =
        ((instruction & 0x0FB00000u) == 0x03200000u) &&
        (((instruction >> 12u) & 0xFu) == 0xFu);
    if (!is_msr_reg && !is_msr_imm) {
        return false;
    }

    const bool use_spsr = (instruction & (1u << 22)) != 0u;
    uint32_t value = 0u;
    if (is_msr_reg) {
        const uint32_t rm = instruction & 0xFu;
        value = ReadArmRegister(rm, fetch_pc);
    } else {
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t rotate = ((instruction >> 8u) & 0xFu) * 2u;
        value = Ror(imm8, rotate);
    }

    const uint32_t field_mask = (instruction >> 16u) & 0xFu;
    uint32_t byte_mask = 0u;
    if ((field_mask & 0x8u) != 0u) {
        byte_mask |= 0xFF000000u;
    }
    if ((field_mask & 0x4u) != 0u) {
        byte_mask |= 0x00FF0000u;
    }
    if ((field_mask & 0x2u) != 0u) {
        byte_mask |= 0x0000FF00u;
    }
    if ((field_mask & 0x1u) != 0u) {
        byte_mask |= 0x000000FFu;
    }
    if (byte_mask == 0u) {
        byte_mask = 0xF0000000u;
    }

    if (!CurrentModeIsPrivileged()) {
        byte_mask &= 0xFFFFFF00u;
    }

    if (use_spsr) {
        if (!ModeHasSpsr(CurrentMode())) {
            cpu_.r[15] = next_pc;
            return true;
        }
        const uint32_t merged_spsr = (ReadCurrentSpsr() & ~byte_mask) | (value & byte_mask);
        if (!WriteCurrentSpsr(merged_spsr)) {
            return false;
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    const uint32_t merged_cpsr = (cpu_.cpsr & ~byte_mask) | (value & byte_mask);
    WriteCpsr(merged_cpsr);
    cpu_.r[15] = next_pc;
    return true;
}


bool ArmCpu::ExecuteArmSingleDataTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool immediate_offset = (instruction & (1u << 25)) == 0;
    const bool pre_index = (instruction & (1u << 24)) != 0;
    const bool add_offset = (instruction & (1u << 23)) != 0;
    const bool byte_transfer = (instruction & (1u << 22)) != 0;
    const bool write_back = (instruction & (1u << 21)) != 0;
    const bool load = (instruction & (1u << 20)) != 0;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t rd = (instruction >> 12) & 0xFu;

    uint32_t offset = 0;
    if (immediate_offset) {
        offset = instruction & 0xFFFu;
    } else {
        bool discard_carry = false;
        offset = DecodeShiftedRegister(instruction, fetch_pc, discard_carry);
    }

    const uint32_t base = ReadArmRegister(rn, fetch_pc);
    uint32_t address = base;
    uint32_t updated_base = base;

    if (pre_index) {
        address = add_offset ? (base + offset) : (base - offset);
        updated_base = address;
    } else {
        updated_base = add_offset ? (base + offset) : (base - offset);
    }

    if (load) {
        uint32_t value = 0;
        if (byte_transfer) {
            value = bus_.Read8(address);
        } else {
            value = Ror(bus_.Read32(address & ~3u), (address & 3u) * 8u); // Misaligned LDR rotates.
        }

        const uint32_t old_pc = cpu_.r[15];
        WriteArmRegister(rd, value);
        if (rd == 15u) {
            RecordPcFlowEvent("ARM.LDR.PC", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
            if (!pre_index || write_back) {
                WriteArmRegister(rn, updated_base);
            }
            return true;
        }
    } else {
        uint32_t value = ReadArmRegister(rd, fetch_pc);
        if (byte_transfer) {
            bus_.Write8(address, static_cast<uint8_t>(value & 0xFFu));
        } else {
            bus_.Write32(address & ~3u, value);
        }
    }

    if (!pre_index || write_back) {
        WriteArmRegister(rn, updated_base);
    }

    cpu_.r[15] = next_pc;
    return true;
}


bool ArmCpu::ExecuteArmHalfwordTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool pre_index = (instruction & (1u << 24)) != 0u;
    const bool add_offset = (instruction & (1u << 23)) != 0u;
    const bool immediate_offset = (instruction & (1u << 22)) != 0u;
    const bool write_back = (instruction & (1u << 21)) != 0u;
    const bool load = (instruction & (1u << 20)) != 0u;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t rd = (instruction >> 12) & 0xFu;
    const uint32_t op = (instruction >> 5) & 0x3u; // 01=H,10=SB,11=SH

    uint32_t offset = 0u;
    if (immediate_offset) {
        offset = ((instruction >> 8u) & 0xFu) << 4u;
        offset |= (instruction & 0xFu);
    } else {
        const uint32_t rm = instruction & 0xFu;
        offset = ReadArmRegister(rm, fetch_pc);
    }

    const uint32_t base = ReadArmRegister(rn, fetch_pc);
    uint32_t address = base;
    uint32_t updated_base = base;

    if (pre_index) {
        address = add_offset ? (base + offset) : (base - offset);
        updated_base = address;
    } else {
        updated_base = add_offset ? (base + offset) : (base - offset);
    }

    if (!load) {
        if (op != 0x1u) {
            return false;
        }
        const uint16_t value = static_cast<uint16_t>(ReadArmRegister(rd, fetch_pc) & 0xFFFFu);
        bus_.Write16(address & ~1u, value);
    } else {
        uint32_t value = 0u;
        switch (op) {
        case 0x1u: // LDRH
            value = bus_.Read16(address & ~1u);
            break;
        case 0x2u: { // LDRSB
            const int8_t signed_byte = static_cast<int8_t>(bus_.Read8(address));
            value = static_cast<uint32_t>(static_cast<int32_t>(signed_byte));
            break;
        }
        case 0x3u: { // LDRSH
            const int16_t signed_half = static_cast<int16_t>(bus_.Read16(address & ~1u));
            value = static_cast<uint32_t>(static_cast<int32_t>(signed_half));
            break;
        }
        default:
            return false;
        }
        const uint32_t old_pc = cpu_.r[15];
        WriteArmRegister(rd, value);
        if (rd == 15u) {
            RecordPcFlowEvent("ARM.LDRH.PC", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
            if (!pre_index || write_back) {
                WriteArmRegister(rn, updated_base);
            }
            return true;
        }
    }

    if (!pre_index || write_back) {
        WriteArmRegister(rn, updated_base);
    }
    cpu_.r[15] = next_pc;
    return true;
}


bool ArmCpu::ExecuteArmMultiply(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool accumulate = (instruction & (1u << 21)) != 0u;
    const bool set_flags = (instruction & (1u << 20)) != 0u;

    const uint32_t rd = (instruction >> 16) & 0xFu;
    const uint32_t rn = (instruction >> 12) & 0xFu;
    const uint32_t rs = (instruction >> 8) & 0xFu;
    const uint32_t rm = instruction & 0xFu;

    if (rd == 15u) {
        return false;
    }

    const uint64_t lhs = static_cast<uint64_t>(ReadArmRegister(rm, fetch_pc));
    const uint64_t rhs = static_cast<uint64_t>(ReadArmRegister(rs, fetch_pc));
    uint64_t wide = lhs * rhs;

    if (accumulate) {
        wide += static_cast<uint64_t>(ReadArmRegister(rn, fetch_pc));
    }

    const uint32_t result = static_cast<uint32_t>(wide & 0xFFFFFFFFu);
    WriteArmRegister(rd, result);

    if (set_flags) {
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | ((result & 0x80000000u) ? (1u << 31) : 0u);
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | ((result == 0u) ? (1u << 30) : 0u);
    }

    cpu_.r[15] = next_pc;
    return true;
}


bool ArmCpu::ExecuteArmMultiplyLong(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool signed_multiply = (instruction & (1u << 22)) != 0u;
    const bool accumulate = (instruction & (1u << 21)) != 0u;
    const bool set_flags = (instruction & (1u << 20)) != 0u;

    const uint32_t rd_hi = (instruction >> 16) & 0xFu;
    const uint32_t rd_lo = (instruction >> 12) & 0xFu;
    const uint32_t rs = (instruction >> 8) & 0xFu;
    const uint32_t rm = instruction & 0xFu;

    if (rd_hi == 15u || rd_lo == 15u || rd_hi == rd_lo) {
        return false;
    }

    uint64_t wide_result = 0;
    if (signed_multiply) {
        const int64_t lhs = static_cast<int64_t>(static_cast<int32_t>(ReadArmRegister(rm, fetch_pc)));
        const int64_t rhs = static_cast<int64_t>(static_cast<int32_t>(ReadArmRegister(rs, fetch_pc)));
        wide_result = static_cast<uint64_t>(lhs * rhs);
    } else {
        const uint64_t lhs = static_cast<uint64_t>(ReadArmRegister(rm, fetch_pc));
        const uint64_t rhs = static_cast<uint64_t>(ReadArmRegister(rs, fetch_pc));
        wide_result = lhs * rhs;
    }

    if (accumulate) {
        const uint64_t current =
            (static_cast<uint64_t>(cpu_.r[rd_hi]) << 32u) | static_cast<uint64_t>(cpu_.r[rd_lo]);
        wide_result += current;
    }

    cpu_.r[rd_lo] = static_cast<uint32_t>(wide_result & 0xFFFFFFFFu);
    cpu_.r[rd_hi] = static_cast<uint32_t>((wide_result >> 32u) & 0xFFFFFFFFu);

    if (set_flags) {
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | ((wide_result & (1ull << 63u)) ? (1u << 31) : 0u);
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | ((wide_result == 0u) ? (1u << 30) : 0u);
    }

    cpu_.r[15] = next_pc;
    return true;
}


bool ArmCpu::ExecuteArmBlockDataTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool pre_index = (instruction & (1u << 24)) != 0u;
    const bool add_offset = (instruction & (1u << 23)) != 0u;
    const bool psr_and_force_user = (instruction & (1u << 22)) != 0u;
    const bool write_back = (instruction & (1u << 21)) != 0u;
    const bool load = (instruction & (1u << 20)) != 0u;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t reg_list = instruction & 0xFFFFu;
    const bool force_user_registers = psr_and_force_user &&
        CurrentModeIsPrivileged() && (reg_list & (1u << 15u)) == 0u;

    if (reg_list == 0u) {
        return false;
    }

    uint32_t reg_count = 0;
    for (uint32_t reg = 0; reg < 16u; ++reg) {
        if ((reg_list & (1u << reg)) != 0u) {
            ++reg_count;
        }
    }

    const uint32_t base = ReadArmRegister(rn, fetch_pc);
    uint32_t start_address = base;
    uint32_t updated_base = base;

    if (add_offset) {
        start_address = pre_index ? (base + 4u) : base;
        updated_base = base + (4u * reg_count);
    } else {
        start_address = pre_index ? (base - (4u * reg_count)) : (base - (4u * (reg_count - 1u)));
        updated_base = base - (4u * reg_count);
    }

    uint32_t address = start_address;
    bool loaded_pc = false;
    uint32_t loaded_pc_value = 0u;
    const uint32_t old_pc = cpu_.r[15];
    for (uint32_t reg = 0; reg < 16u; ++reg) {
        if ((reg_list & (1u << reg)) == 0u) {
            continue;
        }

        if (load) {
            const uint32_t value = bus_.Read32(address & ~3u);
            if (reg == 15u) {
                loaded_pc = true;
                loaded_pc_value = value;
            } else if (force_user_registers) {
                WriteArmUserRegister(reg, value);
            } else {
                WriteArmRegister(reg, value);
            }
        } else {
            const uint32_t value = force_user_registers
                ? ReadArmUserRegister(reg, fetch_pc)
                : ReadArmRegister(reg, fetch_pc);
            bus_.Write32(address & ~3u, value);
        }

        address += 4u;
    }

    // A loaded base register wins over writeback.
    if (write_back && !(load && (reg_list & (1u << rn)) != 0u)) {
        WriteArmRegister(rn, updated_base);
    }

    if (loaded_pc) {
        if (load && psr_and_force_user && ModeHasSpsr(CurrentMode())) {
            const uint32_t restored_cpsr = ReadCurrentSpsr();
            const uint32_t restored_pc = (restored_cpsr & kCpsrThumbBit) != 0u
                ? (loaded_pc_value & ~1u)
                : (loaded_pc_value & ~3u);
            WriteCpsr(restored_cpsr);
            cpu_.r[15] = restored_pc;
        } else {
            cpu_.r[15] = loaded_pc_value & ~3u;
        }
        RecordPcFlowEvent("ARM.LDM.PC", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
    } else {
        cpu_.r[15] = next_pc;
    }
    return true;
}


bool ArmCpu::ExecuteArmSoftwareInterrupt(uint32_t instruction, uint32_t next_pc, std::string& error) {
    const uint8_t swi_imm = static_cast<uint8_t>(instruction & 0xFFu);
    return ExecuteSoftwareInterrupt(swi_imm, next_pc, false, error);
}


} // namespace emugbds::arm
