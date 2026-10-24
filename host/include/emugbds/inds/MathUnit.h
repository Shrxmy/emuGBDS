#pragma once

// ARM9 hardware divider and square root unit (4000280h-40002BFh). Results are
// ready at once; the busy flags never read as set.

#include <cstdint>

namespace emugbds::inds {

class MathUnit {
public:
    void Reset() noexcept { *this = MathUnit{}; }
    // offset = address - 4000280h (word aligned); mask selects the written byte lanes.
    uint32_t ReadWord(uint32_t offset) const noexcept;
    void WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept;

private:
    void Divide() noexcept;
    void SquareRoot() noexcept;

    uint16_t divcnt_ = 0;
    uint64_t numer_ = 0;
    uint64_t denom_ = 0;
    uint64_t result_ = 0;
    uint64_t remainder_ = 0;
    uint16_t sqrtcnt_ = 0;
    uint32_t sqrt_result_ = 0;
    uint64_t sqrt_param_ = 0;
};

} // namespace emugbds::inds
