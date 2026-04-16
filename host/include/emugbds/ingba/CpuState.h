#pragma once

#include <array>
#include <cstdint>

namespace emugbds::ingba {

struct CpuState {
    std::array<uint32_t, 16> r{};
    uint32_t cpsr = 0x0000001Fu;
    bool thumb = false;
};

} // namespace emugbds::ingba
