#include "emugbds/arm/ArmCpu.h"
#include "emugbds/arm/DebugTrace.h"

namespace emugbds::arm {

bool ArmCpu::ExecuteThumbInstruction(uint16_t instruction, uint32_t pc, uint32_t fetch_pc, std::string& error) {
    const uint32_t next_pc = pc + 2u;

    auto logical_shift_left = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            carry_valid = false;
            carry_out = false;
            return value;
        }
        carry_valid = true;
        if (amount < 32u) {
            carry_out = ((value >> (32u - amount)) & 1u) != 0u;
            return value << amount;
        }
        if (amount == 32u) {
            carry_out = (value & 1u) != 0u;
            return 0u;
        }
        carry_out = false;
        return 0u;
    };

    auto logical_shift_right = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            amount = 32u;
        }
        carry_valid = true;
        if (amount < 32u) {
            carry_out = ((value >> (amount - 1u)) & 1u) != 0u;
            return value >> amount;
        }
        if (amount == 32u) {
            carry_out = ((value >> 31u) & 1u) != 0u;
            return 0u;
        }
        carry_out = false;
        return 0u;
    };

    auto arithmetic_shift_right = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            amount = 32u;
        }
        carry_valid = true;
        if (amount < 32u) {
            carry_out = ((value >> (amount - 1u)) & 1u) != 0u;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
        }
        const bool sign = (value & 0x80000000u) != 0u;
        carry_out = sign;
        return sign ? 0xFFFFFFFFu : 0u;
    };

    auto rotate_right = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            carry_valid = false;
            carry_out = false;
            return value;
        }
        carry_valid = true;
        const uint32_t masked = amount & 31u;
        if (masked == 0u) {
            carry_out = ((value >> 31u) & 1u) != 0u;
            return value;
        }
        const uint32_t rotated = Ror(value, masked);
        carry_out = ((rotated >> 31u) & 1u) != 0u;
        return rotated;
    };

    if ((instruction & 0xF800u) == 0xF000u) {
        // The BL prefix lives in LR, as on hardware, so it survives an IRQ between the halves.
        const int32_t upper = SignExtend(instruction & 0x07FFu, 11u) << 12u;
        cpu_.r[14] = fetch_pc + static_cast<uint32_t>(upper);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF800u) == 0xF800u) {
        const uint32_t lower = (instruction & 0x07FFu) << 1u;
        const uint32_t target = cpu_.r[14] + lower;
        cpu_.r[14] = next_pc | 1u;
        const uint32_t old_pc = cpu_.r[15];
        cpu_.r[15] = target & ~1u;
        RecordPcFlowEvent("THUMB.BL", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
        return true;
    }

    if (v5_ && (instruction & 0xF800u) == 0xE800u) { // BLX suffix: like BL, then switch to ARM.
        const uint32_t target = (cpu_.r[14] + ((instruction & 0x07FFu) << 1u)) & ~3u;
        cpu_.r[14] = next_pc | 1u;
        const uint32_t old_pc = cpu_.r[15];
        SetThumbState(false);
        cpu_.r[15] = target;
        RecordPcFlowEvent("THUMB.BLX", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
        return true;
    }

    if ((instruction & 0xF800u) == 0x1800u) {
        const bool immediate = (instruction & (1u << 10)) != 0u;
        const bool subtract = (instruction & (1u << 9)) != 0u;
        const uint32_t op2_index = (instruction >> 6) & 0x7u;
        const uint32_t rs = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t lhs = ReadThumbRegister(rs, fetch_pc);
        const uint32_t rhs = immediate ? op2_index : ReadThumbRegister(op2_index, fetch_pc);

        const AddResult add = subtract ? AddWithCarry(lhs, ~rhs, true) : AddWithCarry(lhs, rhs, false);
        WriteThumbRegister(rd, add.value);
        SetNZFlags(add.value);
        SetCFlag(add.carry);
        SetVFlag(add.overflow);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xE000u) == 0x0000u) {
        const uint32_t op = (instruction >> 11) & 0x3u;
        const uint32_t imm5 = (instruction >> 6) & 0x1Fu;
        const uint32_t rs = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t value = ReadThumbRegister(rs, fetch_pc);
        bool carry_out = false;
        bool carry_valid = false;
        uint32_t result = value;

        if (op == 0u) {
            result = logical_shift_left(value, imm5, carry_out, carry_valid);
        } else if (op == 1u) {
            result = logical_shift_right(value, imm5, carry_out, carry_valid);
        } else if (op == 2u) {
            result = arithmetic_shift_right(value, imm5, carry_out, carry_valid);
        } else {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }

        WriteThumbRegister(rd, result);
        SetNZFlags(result);
        if (carry_valid) {
            SetCFlag(carry_out);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xE000u) == 0x2000u) {
        const uint32_t op = (instruction >> 11) & 0x3u;
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t lhs = ReadThumbRegister(rd, fetch_pc);

        switch (op) {
        case 0x0: // MOV
            WriteThumbRegister(rd, imm8);
            SetNZFlags(imm8);
            break;
        case 0x1: { // CMP
            const AddResult add = AddWithCarry(lhs, ~imm8, true);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x2: { // ADD
            const AddResult add = AddWithCarry(lhs, imm8, false);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x3: { // SUB
            const AddResult add = AddWithCarry(lhs, ~imm8, true);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        default:
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xFC00u) == 0x4000u) {
        const uint32_t op = (instruction >> 6) & 0xFu;
        const uint32_t rs = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t rhs = ReadThumbRegister(rs, fetch_pc);
        const uint32_t lhs = ReadThumbRegister(rd, fetch_pc);
        uint32_t result = 0u;
        if (op == 0x2u || op == 0x3u || op == 0x4u || op == 0x7u) {
            bus_.AddInternal(1u); // LSL/LSR/ASR/ROR by register: +1I
        }

        switch (op) {
        case 0x0: // AND
            result = lhs & rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0x1: // EOR
            result = lhs ^ rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0x2: { // LSL
            bool carry_out = false;
            bool carry_valid = false;
            result = logical_shift_left(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x3: { // LSR
            bool carry_out = false;
            bool carry_valid = false;
            // A zero register amount is no shift; only the immediate form treats 0 as 32.
            result = (rhs & 0xFFu) == 0u ? lhs : logical_shift_right(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x4: { // ASR
            bool carry_out = false;
            bool carry_valid = false;
            result = (rhs & 0xFFu) == 0u ? lhs : arithmetic_shift_right(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x5: { // ADC
            const AddResult add = AddWithCarry(lhs, rhs, (cpu_.cpsr & (1u << 29)) != 0u);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x6: { // SBC
            const AddResult add = AddWithCarry(lhs, ~rhs, (cpu_.cpsr & (1u << 29)) != 0u);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x7: { // ROR
            bool carry_out = false;
            bool carry_valid = false;
            result = rotate_right(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x8: // TST
            result = lhs & rhs;
            SetNZFlags(result);
            break;
        case 0x9: { // NEG
            const AddResult add = AddWithCarry(0u, ~rhs, true);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0xA: { // CMP
            const AddResult add = AddWithCarry(lhs, ~rhs, true);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0xB: { // CMN
            const AddResult add = AddWithCarry(lhs, rhs, false);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0xC: // ORR
            result = lhs | rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0xD: // MUL
            bus_.AddInternal(MultiplyCycles(lhs));
            result = lhs * rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0xE: // BIC
            result = lhs & ~rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0xF: // MVN
            result = ~rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        default:
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xFC00u) == 0x4400u) {
        const uint32_t op = (instruction >> 8) & 0x3u;
        const uint32_t rd = ((instruction >> 4) & 0x8u) | (instruction & 0x7u);
        const uint32_t rs = ((instruction >> 3) & 0x8u) | ((instruction >> 3) & 0x7u);
        const uint32_t lhs = ReadThumbRegister(rd, fetch_pc);
        const uint32_t rhs = ReadThumbRegister(rs, fetch_pc);

        switch (op) {
        case 0x0: { // ADD
            const uint32_t value = lhs + rhs;
            WriteThumbRegister(rd, value);
            if (rd != 15u) {
                cpu_.r[15] = next_pc;
            } else {
                RecordPcFlowEvent("THUMB.ADD.PC", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
            }
            return true;
        }
        case 0x1: { // CMP
            const AddResult add = AddWithCarry(lhs, ~rhs, true);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            cpu_.r[15] = next_pc;
            return true;
        }
        case 0x2: // MOV
            WriteThumbRegister(rd, rhs);
            if (rd != 15u) {
                cpu_.r[15] = next_pc;
            } else {
                RecordPcFlowEvent("THUMB.MOV.PC", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
            }
            return true;
        case 0x3: { // BX, or BLX when MSBd is set (ARMv5)
            if (v5_ && (instruction & 0x0080u) != 0u) {
                cpu_.r[14] = next_pc | 1u; // rhs was read first, so BLX LR works.
            }
            const bool next_thumb = (rhs & 1u) != 0u;
            SetThumbState(next_thumb);
            cpu_.r[15] = next_thumb ? (rhs & ~1u) : (rhs & ~3u);
            RecordPcFlowEvent("THUMB.BX", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
            return true;
        }
        default:
            break;
        }
        return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
    }

    if ((instruction & 0xF800u) == 0x4800u) {
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t address = (fetch_pc & ~3u) + (imm8 << 2u);
        const uint32_t value = bus_.Read32(address & ~3u);
        WriteThumbRegister(rd, value);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0x5000u) {
        const uint32_t rm = (instruction >> 6) & 0x7u;
        const uint32_t rn = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t address = ReadThumbRegister(rn, fetch_pc) + ReadThumbRegister(rm, fetch_pc);

        if ((instruction & (1u << 9)) == 0u) {
            const bool load = (instruction & (1u << 11)) != 0u;
            const bool byte_transfer = (instruction & (1u << 10)) != 0u;
            if (load) {
                const uint32_t value = byte_transfer
                    ? bus_.Read8(address)
                    : Ror(bus_.Read32(address & ~3u), (address & 3u) * 8u);
                WriteThumbRegister(rd, value);
            } else {
                const uint32_t value = ReadThumbRegister(rd, fetch_pc);
                if (byte_transfer) {
                    bus_.Write8(address, static_cast<uint8_t>(value & 0xFFu));
                } else {
                    bus_.Write32(address & ~3u, value);
                }
            }
            cpu_.r[15] = next_pc;
            return true;
        }

        const uint32_t op = (instruction >> 10) & 0x3u;
        switch (op) {
        case 0x0: { // STRH
            const uint16_t value = static_cast<uint16_t>(ReadThumbRegister(rd, fetch_pc) & 0xFFFFu);
            bus_.Write16(address & ~1u, value);
            break;
        }
        case 0x1: { // LDSB
            const int8_t value = static_cast<int8_t>(bus_.Read8(address));
            WriteThumbRegister(rd, static_cast<uint32_t>(static_cast<int32_t>(value)));
            break;
        }
        case 0x2: { // LDRH
            const uint32_t value = bus_.Read16(address & ~1u);
            WriteThumbRegister(rd, value);
            break;
        }
        case 0x3: { // LDSH
            const int16_t value = static_cast<int16_t>(bus_.Read16(address & ~1u));
            WriteThumbRegister(rd, static_cast<uint32_t>(static_cast<int32_t>(value)));
            break;
        }
        default:
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xE000u) == 0x6000u) {
        const bool byte_transfer = (instruction & (1u << 12)) != 0u;
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t imm5 = (instruction >> 6) & 0x1Fu;
        const uint32_t rn = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t offset = byte_transfer ? imm5 : (imm5 << 2u);
        const uint32_t address = ReadThumbRegister(rn, fetch_pc) + offset;

        if (load) {
            const uint32_t value = byte_transfer
                ? bus_.Read8(address)
                : Ror(bus_.Read32(address & ~3u), (address & 3u) * 8u);
            WriteThumbRegister(rd, value);
        } else {
            const uint32_t value = ReadThumbRegister(rd, fetch_pc);
            if (byte_transfer) {
                bus_.Write8(address, static_cast<uint8_t>(value & 0xFFu));
            } else {
                bus_.Write32(address & ~3u, value);
            }
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0x8000u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t imm5 = (instruction >> 6) & 0x1Fu;
        const uint32_t rn = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t address = ReadThumbRegister(rn, fetch_pc) + (imm5 << 1u);

        if (load) {
            const uint32_t value = bus_.Read16(address & ~1u);
            WriteThumbRegister(rd, value);
        } else {
            const uint16_t value = static_cast<uint16_t>(ReadThumbRegister(rd, fetch_pc) & 0xFFFFu);
            bus_.Write16(address & ~1u, value);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0x9000u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t address = cpu_.r[13] + (imm8 << 2u);
        if (load) {
            WriteThumbRegister(rd, Ror(bus_.Read32(address & ~3u), (address & 3u) * 8u));
        } else {
            bus_.Write32(address & ~3u, ReadThumbRegister(rd, fetch_pc));
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0xA000u) {
        const bool use_sp = (instruction & (1u << 11)) != 0u;
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t base = use_sp ? cpu_.r[13] : (fetch_pc & ~3u);
        WriteThumbRegister(rd, base + (imm8 << 2u));
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xFF00u) == 0xB000u) {
        const bool subtract = (instruction & (1u << 7)) != 0u;
        const uint32_t offset = (instruction & 0x7Fu) << 2u;
        cpu_.r[13] = subtract ? (cpu_.r[13] - offset) : (cpu_.r[13] + offset);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF600u) == 0xB400u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const bool include_extra = (instruction & (1u << 8)) != 0u;
        uint32_t reg_list = instruction & 0xFFu;
        if (load && include_extra) {
            reg_list |= (1u << 15);
        } else if (!load && include_extra) {
            reg_list |= (1u << 14);
        }
        if (reg_list == 0u) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }

        uint32_t count = 0u;
        for (uint32_t reg = 0; reg < 16u; ++reg) {
            if ((reg_list & (1u << reg)) != 0u) {
                ++count;
            }
        }

        if (load) {
            uint32_t address = cpu_.r[13];
            bool loaded_pc = false;
            for (uint32_t reg = 0; reg < 16u; ++reg) {
                if ((reg_list & (1u << reg)) == 0u) {
                    continue;
                }
                const uint32_t value = bus_.Read32(address & ~3u);
                if (reg == 15u) {
                    // ARMv4 POP {PC} stays in Thumb and ignores bit 0; ARMv5 interworks.
                    const uint32_t old_pc = cpu_.r[15];
                    LoadPcInterworking(value);
                    loaded_pc = true;
                    RecordPcFlowEvent("THUMB.POP.PC", old_pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
                } else {
                    cpu_.r[reg] = value;
                }
                address += 4u;
            }
            cpu_.r[13] += count * 4u;
            if (!loaded_pc) {
                cpu_.r[15] = next_pc;
            }
        } else {
            uint32_t address = cpu_.r[13] - (count * 4u);
            for (uint32_t reg = 0; reg < 16u; ++reg) {
                if ((reg_list & (1u << reg)) == 0u) {
                    continue;
                }
                bus_.Write32(address & ~3u, cpu_.r[reg]);
                address += 4u;
            }
            cpu_.r[13] -= count * 4u;
            cpu_.r[15] = next_pc;
        }
        return true;
    }

    if ((instruction & 0xF000u) == 0xC000u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t rn = (instruction >> 8) & 0x7u;
        const uint32_t reg_list = instruction & 0xFFu;
        if (reg_list == 0u) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }

        uint32_t count = 0u;
        for (uint32_t reg = 0; reg < 8u; ++reg) {
            if ((reg_list & (1u << reg)) != 0u) {
                ++count;
            }
        }

        uint32_t address = cpu_.r[rn];
        const uint32_t updated_base = cpu_.r[rn] + count * 4u;
        for (uint32_t reg = 0; reg < 8u; ++reg) {
            if ((reg_list & (1u << reg)) == 0u) {
                continue;
            }
            if (load) {
                cpu_.r[reg] = bus_.Read32(address & ~3u);
            } else {
                // ARMv4 stores the written-back base unless the base is the first listed register.
                const bool new_base = !v5_ && reg == rn && (reg_list & ((1u << rn) - 1u)) != 0u;
                bus_.Write32(address & ~3u, new_base ? updated_base : cpu_.r[reg]);
            }
            address += 4u;
        }
        if (!load || (reg_list & (1u << rn)) == 0u) {
            cpu_.r[rn] += count * 4u; // A loaded base register wins over writeback.
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0xD000u) {
        const uint32_t cond = (instruction >> 8) & 0xFu;
        const uint8_t imm8 = static_cast<uint8_t>(instruction & 0xFFu);
        if (cond == 0xFu) {
            if (ExecuteSoftwareInterrupt(imm8, next_pc, true, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::SoftwareInterrupt, next_pc, error);
        }
        if (cond == 0xEu) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        if (ConditionPassed(cond << 28u)) {
            const int32_t offset = SignExtend(imm8, 8u) << 1u;
            cpu_.r[15] = fetch_pc + static_cast<uint32_t>(offset);
            RecordPcFlowEvent("THUMB.B.cond", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
        } else {
            cpu_.r[15] = next_pc;
        }
        return true;
    }

    if ((instruction & 0xF800u) == 0xE000u) {
        const int32_t offset = SignExtend(instruction & 0x07FFu, 11u) << 1u;
        cpu_.r[15] = fetch_pc + static_cast<uint32_t>(offset);
        RecordPcFlowEvent("THUMB.B", pc, cpu_.r[15], instruction, cpu_.cpsr, cpu_.r[14], cpu_.r[13], trace_frame_);
        return true;
    }

    return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
}


} // namespace emugbds::arm
