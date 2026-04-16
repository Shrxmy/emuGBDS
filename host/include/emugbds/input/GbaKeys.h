#pragma once

#include <cstdint>

namespace emugbds::input {

enum class GbaKey : uint8_t {
    A = 0,
    B = 1,
    Select = 2,
    Start = 3,
    Right = 4,
    Left = 5,
    Up = 6,
    Down = 7,
    R = 8,
    L = 9,
};

constexpr uint32_t GbaKeyMask(GbaKey key) {
    return 1u << static_cast<uint8_t>(key);
}

} // namespace emugbds::input
