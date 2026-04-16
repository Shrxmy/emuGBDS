#include "emugbds/ingba/Core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace emugbds::ingba {
namespace {

constexpr uint32_t kBiosBase = 0x00000000u;
constexpr uint32_t kEwramBase = 0x02000000u;
constexpr uint32_t kIwramBase = 0x03000000u;
constexpr uint32_t kRomBase = 0x08000000u;

constexpr size_t kBiosSize = 0x00004000u;
constexpr size_t kEwramSize = 0x00040000u;
constexpr size_t kIwramSize = 0x00008000u;

constexpr uint32_t kCyclesPerFrame = 280896u;

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
    if (address < base) {
        return 0;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return 0;
    }
    return bytes[offset];
}

void WriteToSpan(std::vector<uint8_t>& bytes, uint32_t base, uint32_t address, uint8_t value) {
    if (address < base) {
        return;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return;
    }
    bytes[offset] = value;
}

} // namespace

bool MemoryBus::LoadBios(const std::filesystem::path& bios_path, std::string& error) {
    std::vector<uint8_t> bios_image;
    if (!LoadWholeFile(bios_path, bios_image, error)) {
        return false;
    }
    if (bios_image.size() != kBiosSize) {
        error = "GBA BIOS must be exactly 16 KiB: " + bios_path.string();
        return false;
    }

    bios_ = std::move(bios_image);
    bios_loaded_ = true;
    if (ewram_.empty()) {
        ewram_.assign(kEwramSize, 0);
    }
    if (iwram_.empty()) {
        iwram_.assign(kIwramSize, 0);
    }
    return true;
}

bool MemoryBus::LoadRom(const std::filesystem::path& rom_path, std::string& error) {
    std::vector<uint8_t> rom_image;
    if (!LoadWholeFile(rom_path, rom_image, error)) {
        return false;
    }

    rom_ = std::move(rom_image);
    if (ewram_.empty()) {
        ewram_.assign(kEwramSize, 0);
    }
    if (iwram_.empty()) {
        iwram_.assign(kIwramSize, 0);
    }
    return true;
}

uint8_t MemoryBus::Read8(uint32_t address) const {
    if (address >= kBiosBase && address < kBiosBase + kBiosSize) {
        if (!bios_loaded_) {
            return 0;
        }
        return ReadFromSpan(bios_, kBiosBase, address);
    }
    if (address >= kEwramBase && address < kEwramBase + kEwramSize) {
        return ReadFromSpan(ewram_, kEwramBase, address);
    }
    if (address >= kIwramBase && address < kIwramBase + kIwramSize) {
        return ReadFromSpan(iwram_, kIwramBase, address);
    }
    if (address >= kRomBase && !rom_.empty()) {
        const uint32_t rom_offset = (address - kRomBase) % static_cast<uint32_t>(rom_.size());
        return rom_[rom_offset];
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
    if (address >= kEwramBase && address < kEwramBase + kEwramSize) {
        WriteToSpan(ewram_, kEwramBase, address, value);
        return;
    }
    if (address >= kIwramBase && address < kIwramBase + kIwramSize) {
        WriteToSpan(iwram_, kIwramBase, address, value);
        return;
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

bool Core::Initialize(std::string& error) {
    (void)error;
    initialized_ = true;
    frame_counter_ = 0;
    unknown_arm_instruction_count_ = 0;
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
    while (cycles < kCyclesPerFrame && !halted_) {
        if (!StepArm(error)) {
            return false;
        }
        ++cycles;
    }

    ++frame_counter_;
    return true;
}

bool Core::Reset(std::string& error) {
    (void)error;
    cpu_ = {};
    cpu_.cpsr = 0x0000001Fu;
    cpu_.thumb = false;
    cpu_.r[15] = bus_.HasBios() ? kBiosBase : kRomBase;
    halted_ = false;
    return true;
}

bool Core::StepArm(std::string& error) {
    if (cpu_.thumb) {
        const uint16_t instruction = bus_.Read16(cpu_.r[15]);
        if (instruction != 0x46C0u) {
            ++unknown_arm_instruction_count_;
        }
        cpu_.r[15] += 2;
        return true;
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
        cpu_.thumb = (target & 1u) != 0;
        cpu_.r[15] = target & ~1u;
        return true;
    }

    if ((instruction & 0x0E000000u) == 0x0A000000u) {
        const int32_t signed_offset = static_cast<int32_t>(SignExtend24(instruction & 0x00FFFFFFu) << 2);
        if ((instruction & (1u << 24)) != 0u) {
            cpu_.r[14] = next_pc;
        }
        cpu_.r[15] = static_cast<uint32_t>(static_cast<int32_t>(fetch_pc) + signed_offset);
        return true;
    }

    const uint32_t top2 = (instruction >> 26) & 0x3u;
    if (top2 == 0x0u) {
        if ((instruction & 0x0FC000F0u) == 0x00000090u) {
            // Multiply class not implemented in this milestone.
        } else if (ExecuteArmDataProcessing(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
    } else if (top2 == 0x1u) {
        if (ExecuteArmSingleDataTransfer(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
    }

    if (instruction == 0xE1A00000u || instruction == 0xE320F000u) {
        // NOP forms: MOV r0, r0 and YIELD as no-op.
        cpu_.r[15] = next_pc;
        return true;
    }

    ++unknown_arm_instruction_count_;
    cpu_.r[15] = next_pc;
    if (unknown_arm_instruction_count_ > 10'000'000u) {
        halted_ = true;
        error = "In-house GBA core halted due to unsupported instruction volume";
        return false;
    }
    return true;
}

bool Core::ExecuteArmDataProcessing(
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
        WriteArmRegister(rd, result);
        if (rd == 15u) {
            return true;
        }
    }

    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmSingleDataTransfer(
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
            value = bus_.Read32(address & ~3u);
        }

        WriteArmRegister(rd, value);
        if (rd == 15u) {
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
