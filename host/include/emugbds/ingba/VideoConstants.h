#pragma once

// LCD timing, video-register, and pixel-format constants for the in-house GBA core.

#include <cstdint>

namespace emugbds::ingba {

inline constexpr uint32_t kCyclesPerFrame = 280896u;
inline constexpr uint32_t kCyclesPerScanline = 1232u;
inline constexpr uint32_t kVisibleScanlines = 160u;
inline constexpr uint32_t kTotalScanlines = 228u;
inline constexpr uint32_t kDispCntAddress = 0x04000000u;
inline constexpr uint32_t kDispStatAddress = 0x04000004u;
inline constexpr uint32_t kVCountAddress = 0x04000006u;
inline constexpr uint32_t kBgCntAddressBase = 0x04000008u;
inline constexpr uint32_t kBgHofsAddressBase = 0x04000010u;
inline constexpr uint32_t kBgVofsAddressBase = 0x04000012u;
inline constexpr uint32_t kModeMask = 0x0007u;
inline constexpr uint16_t kDispStatVBlank = 0x0001u;
inline constexpr uint16_t kDispStatHBlank = 0x0002u;
inline constexpr uint16_t kDispStatVCounter = 0x0004u;
inline constexpr uint16_t kDispStatVBlankIrqEnable = 0x0008u;
inline constexpr uint16_t kDispStatHBlankIrqEnable = 0x0010u;
inline constexpr uint16_t kDispStatVCounterIrqEnable = 0x0020u;

inline uint8_t Expand5To8(uint8_t value5) {
    return static_cast<uint8_t>((value5 << 3u) | (value5 >> 2u));
}

inline uint32_t Bgr555ToAbgr8888(uint16_t bgr555) {
    const uint8_t r5 = static_cast<uint8_t>(bgr555 & 0x1Fu);
    const uint8_t g5 = static_cast<uint8_t>((bgr555 >> 5u) & 0x1Fu);
    const uint8_t b5 = static_cast<uint8_t>((bgr555 >> 10u) & 0x1Fu);
    const uint8_t r8 = Expand5To8(r5);
    const uint8_t g8 = Expand5To8(g5);
    const uint8_t b8 = Expand5To8(b5);
    return 0xFF000000u |
        (static_cast<uint32_t>(b8) << 16u) |
        (static_cast<uint32_t>(g8) << 8u) |
        static_cast<uint32_t>(r8);
}

} // namespace emugbds::ingba
