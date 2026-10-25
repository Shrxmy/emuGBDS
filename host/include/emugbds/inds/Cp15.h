#pragma once

// ARM946E-S system control coprocessor (CP15) of the DS ARM9: ID registers, the
// control register, TCM regions, and the wait-for-interrupt halt. Cache and
// protection-unit registers are stored but have no effect.

#include <algorithm>
#include <cstdint>
#include <map>

namespace emugbds::inds {

class Cp15 {
public:
    static constexpr uint32_t kItcmPhysicalSize = 0x8000u;
    static constexpr uint32_t kDtcmPhysicalSize = 0x4000u;

    void Reset() noexcept;
    // MRC/MCR with opcode1 = 0. Return false for registers that do not exist.
    bool Read(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t& value) const;
    bool Write(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t value);

    uint32_t control() const noexcept { return control_; }
    bool itcm_enabled() const noexcept { return (control_ & (1u << 18)) != 0u; }
    bool dtcm_enabled() const noexcept { return (control_ & (1u << 16)) != 0u; }
    // Load mode makes a TCM write-only: reads fall through to the memory underneath.
    bool itcm_load_mode() const noexcept { return (control_ & (1u << 19)) != 0u; }
    bool dtcm_load_mode() const noexcept { return (control_ & (1u << 17)) != 0u; }
    // ITCM base is fixed at 0 on the DS; only its virtual size is programmable.
    uint64_t itcm_size() const noexcept { return RegionSize(itcm_region_); }
    uint32_t dtcm_base() const noexcept { return dtcm_region_ & 0xFFFFF000u; }
    uint64_t dtcm_size() const noexcept { return RegionSize(dtcm_region_); }
    uint32_t exception_base() const noexcept { return (control_ & (1u << 13)) != 0u ? 0xFFFF0000u : 0u; }
    // Control bit 15 restores ARMv4 behavior for loads into PC (no Thumb interworking).
    bool load_interworking() const noexcept { return (control_ & (1u << 15)) == 0u; }
    // Changes whenever the control register or a TCM region changes (anything that moves the memory map).
    uint32_t map_revision() const noexcept { return map_revision_; }
    bool ConsumeHaltRequest() noexcept {
        const bool requested = halt_requested_;
        halt_requested_ = false;
        return requested;
    }

private:
    // 64-bit so the largest encoding (N=23) covers the whole 4 GiB address space.
    static uint64_t RegionSize(uint32_t region) noexcept {
        const uint32_t shift = std::min((region >> 1u) & 0x1Fu, 23u);
        return uint64_t{512u} << shift;
    }

    uint32_t control_ = 0u;
    uint32_t itcm_region_ = 0u;
    uint32_t dtcm_region_ = 0u;
    bool halt_requested_ = false;
    uint32_t map_revision_ = 0u;
    std::map<uint32_t, uint32_t> stored_; // Cache/PU registers: kept for read-back only.
};

} // namespace emugbds::inds
