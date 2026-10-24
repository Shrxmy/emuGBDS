#include "emugbds/inds/Cp15.h"

namespace emugbds::inds {
namespace {

constexpr uint32_t kMainId = 0x41059461u;      // ARMv5TE ARM946 rev 1
constexpr uint32_t kCacheType = 0x0F0D2112u;   // 8 KiB code, 4 KiB data, 32-byte lines
constexpr uint32_t kTcmSize = 0x00140180u;     // ITCM 32 KiB, DTCM 16 KiB
constexpr uint32_t kControlWritable = 0x000FF085u; // bits 0, 2, 7, 12-19
constexpr uint32_t kControlFixed = 0x00000078u;    // bits 3-6 always set

uint32_t Key(uint32_t crn, uint32_t crm, uint32_t opcode2) {
    return (crn << 8u) | (crm << 4u) | opcode2;
}

} // namespace

void Cp15::Reset() noexcept {
    control_ = kControlFixed | (1u << 13); // The DS boots with exception vectors at FFFF0000h.
    itcm_region_ = 0u;
    dtcm_region_ = 0u;
    halt_requested_ = false;
    stored_.clear();
}


bool Cp15::Read(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t& value) const {
    if (opcode1 != 0u) {
        return false;
    }
    if (crn == 0u && crm == 0u) {
        value = opcode2 == 1u ? kCacheType : opcode2 == 2u ? kTcmSize : kMainId; // 3..7 mirror the main ID.
        return true;
    }
    if (crn == 1u && crm == 0u && opcode2 == 0u) {
        value = control_;
        return true;
    }
    if (crn == 9u && crm == 1u) {
        value = opcode2 == 0u ? dtcm_region_ : (itcm_region_ & 0x3Eu);
        return true;
    }
    if (crn == 7u) {
        value = 0u; // Cache commands are write-only.
        return true;
    }
    const auto it = stored_.find(Key(crn, crm, opcode2));
    value = it != stored_.end() ? it->second : 0u;
    return true;
}


bool Cp15::Write(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t value) {
    if (opcode1 != 0u) {
        return false;
    }
    if (crn == 0u) {
        return true; // ID registers are read-only.
    }
    if (crn == 1u && crm == 0u && opcode2 == 0u) {
        control_ = (value & kControlWritable) | kControlFixed;
        return true;
    }
    if (crn == 9u && crm == 1u) {
        (opcode2 == 0u ? dtcm_region_ : itcm_region_) = value & 0xFFFFF03Eu;
        return true;
    }
    if (crn == 7u) {
        if ((crm == 0u && opcode2 == 4u) || (crm == 8u && opcode2 == 2u)) {
            halt_requested_ = true; // Wait for interrupt.
        }
        return true; // Cache maintenance has no effect without a cache model.
    }
    stored_[Key(crn, crm, opcode2)] = value;
    return true;
}

} // namespace emugbds::inds
