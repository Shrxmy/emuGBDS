#pragma once

// ARM interpreter shared by the in-house GBA core and the DS CPUs: ARMv4T for the
// ARM7TDMI (GBA, DS ARM7) and ARMv5TE for the ARM946E-S (DS ARM9).
// Memory goes through IBus; system-specific behavior goes through ICpuHost.

#include "emugbds/arm/Bus.h"
#include "emugbds/arm/CpuState.h"

#include <cstdint>
#include <string>

namespace emugbds::arm {

enum class ArmArchitecture {
    V4T,  // ARM7TDMI
    V5TE, // ARM946E-S
};

class ICpuHost {
public:
    virtual ~ICpuHost() = default;
    // Called for every SWI: the host enters its BIOS vector or emulates the call.
    // Returns false when the call is unsupported.
    virtual bool OnSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) = 0;
    // MRC/MCR (ARMv5TE only). Return false when the coprocessor or register does not exist.
    virtual bool OnCoprocessorRead(uint32_t /*coprocessor*/, uint32_t /*opcode1*/, uint32_t /*crn*/,
                                   uint32_t /*crm*/, uint32_t /*opcode2*/, uint32_t& /*value*/) {
        return false;
    }
    virtual bool OnCoprocessorWrite(uint32_t /*coprocessor*/, uint32_t /*opcode1*/, uint32_t /*crn*/,
                                    uint32_t /*crm*/, uint32_t /*opcode2*/, uint32_t /*value*/) {
        return false;
    }
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

    ArmCpu(IBus& bus, ICpuHost& host, ArmArchitecture architecture) noexcept
        : bus_(bus), host_(host), v5_(architecture == ArmArchitecture::V5TE) {}
    // Cycles the instructions since the last call took (at least 1 per instruction), and resets the count.
    uint32_t TakeCycles() noexcept { return bus_.TakeCycles(); }
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
    // ARMv5 only: false restores ARMv4 loads into PC (the ARM946 CP15 control bit 15).
    void set_load_interworking(bool enabled) noexcept { load_interworking_ = enabled; }

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

    bool ExecuteNext(std::string& error);
    // Multiplier timing: m = 1-4 by how many top bytes of the multiplier are all 0s or all 1s (GBATEK).
    static uint32_t MultiplyCycles(uint32_t multiplier) noexcept {
        const uint32_t top = multiplier ^ static_cast<uint32_t>(static_cast<int32_t>(multiplier) >> 31);
        return top < 0x100u ? 1u : top < 0x10000u ? 2u : top < 0x1000000u ? 3u : 4u;
    }
    bool ExecuteThumbInstruction(uint16_t instruction, uint32_t pc, uint32_t fetch_pc, std::string& error);
    bool ExecuteArmPsrTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmDataProcessing(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSingleDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmHalfwordTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmMultiply(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmMultiplyLong(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmBlockDataTransfer(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    bool ExecuteArmSoftwareInterrupt(uint32_t instruction, uint32_t next_pc, std::string& error);
    bool ExecuteArmSwap(uint32_t instruction, uint32_t next_pc);
    // ARMv5TE: cond=1111 space (BLX immediate, PLD).
    bool ExecuteArmUnconditional(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error);
    // ARMv5TE: BLX reg, CLZ, QADD family, halfword multiplies, MRC/MCR. Sets handled when it decoded one.
    bool ExecuteArmV5Instruction(uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, bool& handled);
    // Writes a loaded value to PC; ARMv5 switches to Thumb when bit 0 is set, ARMv4 keeps the state.
    void LoadPcInterworking(uint32_t value);
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

    // The system bus seen through the CPU's cycle counter: every access is charged the bus's
    // AccessCycles, sequential when it continues the previous access. One load instruction (LDR,
    // LDM, SWP, POP...) also costs one internal cycle, charged on its first data read.
    class TimedBus {
    public:
        explicit TimedBus(IBus& bus) noexcept : bus_(bus) {}
        uint8_t Read8(uint32_t address) const { ChargeRead(address, 1u); return bus_.Read8(address); }
        uint16_t Read16(uint32_t address) const { ChargeRead(address, 2u); return bus_.Read16(address); }
        uint32_t Read32(uint32_t address) const { ChargeRead(address, 4u); return bus_.Read32(address); }
        void Write8(uint32_t address, uint8_t value) { Charge(address, 1u, false); bus_.Write8(address, value); }
        void Write16(uint32_t address, uint16_t value) { Charge(address, 2u, false); bus_.Write16(address, value); }
        void Write32(uint32_t address, uint32_t value) { Charge(address, 4u, false); bus_.Write32(address, value); }
        uint16_t Fetch16(uint32_t address) const {
            loaded_ = false; // a new instruction
            Charge(address, 2u, true);
            return bus_.Read16(address);
        }
        uint32_t Fetch32(uint32_t address) const {
            loaded_ = false;
            Charge(address, 4u, true);
            return bus_.Read32(address);
        }
        // A taken branch refills the pipeline: one more (sequential) opcode fetch at the target.
        void Refill(uint32_t target, uint32_t width) const { cycles_ += Cost(target + width, width, true, true); }
        void AddInternal(uint32_t cycles) const noexcept { cycles_ += cycles; }
        uint32_t TakeCycles() noexcept {
            const uint32_t cycles = cycles_ == 0u ? 1u : cycles_;
            cycles_ = 0u;
            return cycles;
        }

    private:
        uint32_t Cost(uint32_t address, uint32_t width, bool sequential, bool code) const {
            return times_ != nullptr ? TableCost(*times_, address, width, sequential, code)
                                     : UntabledCost(address, width, sequential, code);
        }
        static uint32_t TableCost(const AccessTimes& times, uint32_t address, uint32_t width, bool sequential, bool code) {
            if (address < times.itcm_end || (!code && address - times.dtcm_base < times.dtcm_size)) {
                return times.tcm_cost;
            }
            return times.cost[(address >> 24u) & 0xFu][code ? 1 : 0][sequential ? 1 : 0][width == 4u ? 1 : 0];
        }
        // Looks the table up on first use (the table lives in the bus, so its address is stable);
        // buses without one answer through AccessCycles.
        uint32_t UntabledCost(uint32_t address, uint32_t width, bool sequential, bool code) const {
            if (!times_checked_) {
                times_checked_ = true;
                times_ = bus_.access_times();
                if (times_ != nullptr) {
                    return TableCost(*times_, address, width, sequential, code);
                }
            }
            return bus_.AccessCycles(address, width, sequential, code);
        }
        void Charge(uint32_t address, uint32_t width, bool code) const {
            cycles_ += Cost(address, width, address == next_, code);
            next_ = address + width;
        }
        void ChargeRead(uint32_t address, uint32_t width) const {
            if (!loaded_) {
                loaded_ = true;
                cycles_ += 1u; // the load's internal cycle
            }
            Charge(address, width, false);
        }

        IBus& bus_;
        mutable uint32_t cycles_ = 0u;
        mutable uint32_t next_ = 0xFFFFFFFFu; // the address a sequential access would use
        mutable bool loaded_ = false;
        mutable const AccessTimes* times_ = nullptr;
        mutable bool times_checked_ = false;
    };

    TimedBus bus_;
    ICpuHost& host_;
    bool v5_ = false;
    bool load_interworking_ = true;
    CpuState cpu_{};
    uint64_t trace_frame_ = 0;
    uint64_t unknown_arm_instruction_count_ = 0;
    UnsupportedArmStats unsupported_arm_stats_{};
};

} // namespace emugbds::arm
