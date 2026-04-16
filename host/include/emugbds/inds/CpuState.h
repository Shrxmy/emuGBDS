#pragma once

#include <array>
#include <cstdint>

namespace emugbds::inds {

struct CpuState {
    std::array<uint32_t, 16> r{};
    uint32_t cpsr = 0x0000001Fu;
    bool thumb = false;
    uint64_t unknown_instruction_count = 0;
};

} // namespace emugbds::inds
