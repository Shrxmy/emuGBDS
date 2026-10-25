#pragma once

#include <cstdint>

namespace emugbds::arm {

// A bus's access costs as a lookup table, so the CPU can charge each access without a call:
// cost[address bits 24-27][code][sequential][32-bit], plus the ARM9's tightly coupled memory.
// The bus rebuilds it in place whenever its timing settings change.
struct AccessTimes {
    uint8_t cost[16][2][2][2] = {};
    uint32_t itcm_end = 0;   // [0, itcm_end): ITCM (code and data)
    uint32_t dtcm_base = 0;  // [dtcm_base, dtcm_base + dtcm_size): DTCM (data only)
    uint32_t dtcm_size = 0;
    uint8_t tcm_cost = 1;
};

class IBus {
public:
    virtual ~IBus() = default;

    virtual uint8_t Read8(uint32_t address) const = 0;
    virtual uint16_t Read16(uint32_t address) const = 0;
    virtual uint32_t Read32(uint32_t address) const = 0;
    virtual void Write8(uint32_t address, uint8_t value) = 0;
    virtual void Write16(uint32_t address, uint16_t value) = 0;
    virtual void Write32(uint32_t address, uint32_t value) = 0;
    // CPU clock cycles one access takes (GBATEK "Instruction Cycle Times": 1N or 1S plus the region's
    // waitstates). `width` is 1, 2 or 4 bytes; `sequential` continues the previous access; `code` is an
    // opcode fetch. Buses without a timing model cost 1 cycle per access.
    virtual uint32_t AccessCycles(uint32_t address, uint32_t width, bool sequential, bool code) const {
        (void)address;
        (void)width;
        (void)sequential;
        (void)code;
        return 1u;
    }
    // The same costs as a table the CPU reads directly, or nullptr to call AccessCycles instead.
    virtual const AccessTimes* access_times() const { return nullptr; }
};

} // namespace emugbds::arm
