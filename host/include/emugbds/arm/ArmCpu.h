#pragma once

// ARM7TDMI interpreter shared by the in-house GBA core and, later, the DS CPUs.
// Memory goes through IBus; system-specific behavior goes through ICpuHost.

#include "emugbds/arm/Bus.h"
#include "emugbds/arm/CpuState.h"

#include <cstdint>
#include <string>

namespace emugbds::arm {

class ICpuHost {
public:
    virtual ~ICpuHost() = default;
    // Called for every SWI: the host enters its BIOS vector or emulates the call.
    // Returns false when the call is unsupported.
    virtual bool OnSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) = 0;
};

class ArmCpu {
public:
    struct UnsupportedArmStats {
        uint64_t generic = 0;
        uint64_t thumb = 0;
        uint64_t multiply = 0;
        uint64_t block_transfer = 0;
        uint64_t software_interrupt = 0;
    };

    ArmCpu(IBus& bus, ICpuHost& host) noexcept : bus_(bus), host_(host) {}
    ArmCpu(const ArmCpu&) = delete;
    ArmCpu& operator=(const ArmCpu&) = delete;

    // Clears registers and diagnostics counters; the caller sets the reset CPSR/PC.
    void Reset() noexcept;
    // Executes one instruction. Interrupts are the caller's job (see EnterException).
    bool Step(std::string& error);

    CpuState& state() noexcept { return cpu_; }
    const CpuState& state() const noexcept { return cpu_; }
    uint64_t unknown_arm_instruction_count() const noexcept { return unknown_arm_instruction_count_; }
    const UnsupportedArmStats& unsupported_arm_stats() const noexcept { return unsupported_arm_stats_; }
    // Frame number stamped on debug trace events.
    void set_trace_frame(uint64_t frame) noexcept { trace_frame_ = frame; }

    void SetThumbState(bool enabled) noexcept;
    void WriteCpsr(uint32_t value) noexcept;
    void WriteSpsrForMode(uint32_t mode, uint32_t value) noexcept;
    void EnterException(
        uint32_t target_mode,
        uint32_t handler_address,
        bool handler_thumb,
        uint32_t return_address,
        bool mask_irq) noexcept;

private:
    enum class UnsupportedArmClass {
        Generic,
        Thumb,
        Multiply,
        BlockTransfer,
        SoftwareInterrupt,
    };

    struct AddResult {
        uint32_t value = 0;
        bool carry = false;
        bool overflow = false;
    };

    bool ExecuteThumbInstruction(uint16_t instruction, uint32_t pc, uint32_t fetch_pc, std::string& error);
    bool ExecuteArmPsrTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmDataProcessing(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSingleDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmHalfwordTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmMultiply(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmMultiplyLong(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmBlockDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSoftwareInterrupt(uint32_t instruction, uint32_t next_pc, std::string& error);
    bool ExecuteSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) {
        return host_.OnSoftwareInterrupt(swi_imm, next_pc, from_thumb, error);
    }
    bool HandleUnsupportedArm(UnsupportedArmClass unsupported_class, uint32_t next_pc, std::string& error);
    void RecordUnsupportedArm(UnsupportedArmClass unsupported_class);
    const char* DominantUnsupportedClassName() const noexcept;
    bool DecodeArmOperand2(uint32_t instruction, uint32_t fetch_pc, uint32_t& operand2, bool& shifter_carry) const;
    uint32_t DecodeShiftedRegister(uint32_t instruction, uint32_t fetch_pc, bool& shifter_carry) const;
    uint32_t ReadArmRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteArmRegister(uint32_t reg_index, uint32_t value);
    uint32_t ReadArmUserRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteArmUserRegister(uint32_t reg_index, uint32_t value);
    uint32_t ReadThumbRegister(uint32_t reg_index, uint32_t fetch_pc) const;
    void WriteThumbRegister(uint32_t reg_index, uint32_t value);
    void SetNFlag(bool value);
    void SetZFlag(bool value);
    void SetCFlag(bool value);
    void SetVFlag(bool value);
    void SetNZFlags(uint32_t value);
    bool ConditionPassed(uint32_t instruction) const;
    static uint32_t Ror(uint32_t value, uint32_t amount);
    static AddResult AddWithCarry(uint32_t lhs, uint32_t rhs, bool carry_in);
    static uint32_t SignExtend24(uint32_t value);
    static int32_t SignExtend(uint32_t value, uint32_t bits);
    static bool IsValidCpuMode(uint32_t mode) noexcept;
    static bool ModeHasSpsr(uint32_t mode) noexcept;
    uint32_t CurrentMode() const noexcept;
    bool CurrentModeIsPrivileged() const noexcept;
    void SaveBankedRegistersForMode(uint32_t mode) noexcept;
    void RestoreBankedRegistersForMode(uint32_t mode) noexcept;
    void SwitchCpuMode(uint32_t new_mode) noexcept;
    uint32_t ReadSpsrForMode(uint32_t mode) const noexcept;
    uint32_t ReadCurrentSpsr() const noexcept;
    bool WriteCurrentSpsr(uint32_t value) noexcept;

    IBus& bus_;
    ICpuHost& host_;
    CpuState cpu_{};
    uint64_t trace_frame_ = 0;
    uint64_t unknown_arm_instruction_count_ = 0;
    UnsupportedArmStats unsupported_arm_stats_{};
};

} // namespace emugbds::arm
