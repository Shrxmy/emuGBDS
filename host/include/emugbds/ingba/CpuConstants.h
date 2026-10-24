#pragma once

// CPU mode and status-register constants for the in-house GBA core.

#include <cstdint>

namespace emugbds::ingba {

inline constexpr uint32_t kSwiVectorAddress = 0x00000008u;
inline constexpr uint32_t kCpuModeMask = 0x1Fu;
inline constexpr uint32_t kCpuModeUser = 0x10u;
inline constexpr uint32_t kCpuModeFiq = 0x11u;
inline constexpr uint32_t kCpuModeIrq = 0x12u;
inline constexpr uint32_t kCpuModeSupervisor = 0x13u;
inline constexpr uint32_t kCpuModeAbort = 0x17u;
inline constexpr uint32_t kCpuModeUndefined = 0x1Bu;
inline constexpr uint32_t kCpuModeSystem = 0x1Fu;
inline constexpr uint32_t kCpsrThumbBit = 1u << 5u;
inline constexpr uint32_t kCpsrIrqDisableBit = 1u << 7u;

} // namespace emugbds::ingba
