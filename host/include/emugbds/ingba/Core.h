#pragma once

#include "emugbds/ingba/CpuState.h"
#include "emugbds/ingba/MemoryBus.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace emugbds::ingba {

class Core {
public:
    bool Initialize(std::string& error);
    bool LoadBios(const std::filesystem::path& bios_path, std::string& error);
    bool LoadRom(const std::filesystem::path& rom_path, std::string& error);
    bool RunFrame(std::string& error);
    bool Reset(std::string& error);
    bool IsRunning() const noexcept { return initialized_ && rom_loaded_ && !halted_; }

    const CpuState& cpu() const noexcept { return cpu_; }
    uint64_t frame_counter() const noexcept { return frame_counter_; }
    uint64_t unknown_arm_instruction_count() const noexcept { return unknown_arm_instruction_count_; }

private:
    struct AddResult {
        uint32_t value = 0;
        bool carry = false;
        bool overflow = false;
    };

    bool StepArm(std::string& error);
    bool ExecuteArmDataProcessing(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSingleDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool DecodeArmOperand2(uint32_t instruction, uint32_t fetch_pc, uint32_t& operand2, bool& shifter_carry) const;
    uint32_t DecodeShiftedRegister(uint32_t instruction, uint32_t fetch_pc, bool& shifter_carry) const;
    uint32_t ReadArmRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteArmRegister(uint32_t reg_index, uint32_t value);
    bool ConditionPassed(uint32_t instruction) const;
    static uint32_t Ror(uint32_t value, uint32_t amount);
    static AddResult AddWithCarry(uint32_t lhs, uint32_t rhs, bool carry_in);
    static uint32_t SignExtend24(uint32_t value);

    MemoryBus bus_{};
    CpuState cpu_{};
    bool initialized_ = false;
    bool rom_loaded_ = false;
    bool halted_ = false;
    uint64_t frame_counter_ = 0;
    uint64_t unknown_arm_instruction_count_ = 0;
};

} // namespace emugbds::ingba
