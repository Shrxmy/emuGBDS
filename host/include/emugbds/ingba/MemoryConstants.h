#pragma once

// Address map, storage sizes, and memory-file helpers for the in-house GBA core.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace emugbds::ingba {

inline constexpr uint32_t kBiosBase = 0x00000000u;
inline constexpr uint32_t kEwramBase = 0x02000000u;
inline constexpr uint32_t kIwramBase = 0x03000000u;
inline constexpr uint32_t kIoBase = 0x04000000u;
inline constexpr uint32_t kPaletteBase = 0x05000000u;
inline constexpr uint32_t kVramBase = 0x06000000u;
inline constexpr uint32_t kOamBase = 0x07000000u;
inline constexpr uint32_t kRomBase = 0x08000000u;
inline constexpr uint32_t kSaveBase = 0x0E000000u;
inline constexpr uint32_t kSaveEnd = 0x10000000u; // SRAM/FLASH mirrors fill E000000h-FFFFFFFh

inline constexpr size_t kBiosSize = 0x00004000u;
inline constexpr size_t kEwramSize = 0x00040000u;
inline constexpr size_t kIwramSize = 0x00008000u;
inline constexpr size_t kIoSize = 0x00000400u;

inline constexpr uint32_t kKeyInputAddress = 0x04000130u;
inline constexpr uint32_t kInterruptEnableAddress = 0x04000200u;
inline constexpr uint32_t kInterruptFlagAddress = 0x04000202u;
inline constexpr uint32_t kWaitCntAddress = 0x04000204u;
inline constexpr uint32_t kInterruptMasterEnableAddress = 0x04000208u;
inline constexpr uint32_t kIrqCallbackAddress = 0x03007FFCu;
inline constexpr uint32_t kBiosIrqFlagsMirrorAddress = 0x03FFFFF8u;
inline constexpr uint32_t kBiosIrqFlagsMirrorIwramAddress = 0x03007FF8u;
inline constexpr uint32_t kPostFlgAddress = 0x04000300u;

inline constexpr std::array<uint32_t, 4u> kDmaBaseAddresses = {
    0x040000B0u,
    0x040000BCu,
    0x040000C8u,
    0x040000D4u,
};
inline constexpr std::array<uint32_t, 4u> kDmaControlHighByteAddresses = {
    0x040000BBu,
    0x040000C7u,
    0x040000D3u,
    0x040000DFu,
};
inline constexpr uint16_t kDmaEnableBit = 0x8000u;
inline constexpr uint16_t kDmaStartTimingMask = 0x3000u;
inline constexpr uint16_t kDmaTransfer32Bit = 0x0400u;
inline constexpr uint16_t kDmaRepeatBit = 0x0200u;
inline constexpr uint16_t kDmaIrqBit = 0x4000u;

// VRAM is physically 96 KiB in a 128 KiB repeating aperture. The final
// 32 KiB of that aperture mirrors the physical upper bank.
inline constexpr uint32_t NormalizeVramOffset(uint32_t address) noexcept {
    uint32_t offset = (address - kVramBase) & 0x0001FFFFu;
    if (offset >= 0x00018000u) {
        offset -= 0x00008000u;
    }
    return offset;
}

inline bool LoadWholeFile(
    const std::filesystem::path& path,
    std::vector<uint8_t>& out_bytes,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open file: " + path.string();
        return false;
    }

    out_bytes.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (out_bytes.empty()) {
        error = "File is empty: " + path.string();
        return false;
    }
    return true;
}

inline uint8_t ReadFromSpan(
    const std::vector<uint8_t>& bytes,
    uint32_t base,
    uint32_t address) {
    if (address < base) {
        return 0u;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return 0u;
    }
    return bytes[offset];
}

inline void WriteToSpan(
    std::vector<uint8_t>& bytes,
    uint32_t base,
    uint32_t address,
    uint8_t value) {
    if (address < base) {
        return;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return;
    }
    bytes[offset] = value;
}

inline uint8_t ReadFromMirroredSpan(
    const std::vector<uint8_t>& bytes,
    uint32_t base,
    uint32_t address) {
    if (address < base || bytes.empty()) {
        return 0u;
    }
    const uint32_t offset = address - base;
    return bytes[offset % static_cast<uint32_t>(bytes.size())];
}

inline void WriteToMirroredSpan(
    std::vector<uint8_t>& bytes,
    uint32_t base,
    uint32_t address,
    uint8_t value) {
    if (address < base || bytes.empty()) {
        return;
    }
    const uint32_t offset = address - base;
    bytes[offset % static_cast<uint32_t>(bytes.size())] = value;
}

template <size_t N>
inline uint8_t ReadFromMirroredArray(
    const std::array<uint8_t, N>& bytes,
    uint32_t base,
    uint32_t address) {
    if (address < base) {
        return 0u;
    }
    const uint32_t offset = address - base;
    return bytes[offset % static_cast<uint32_t>(N)];
}

template <size_t N>
inline void WriteToMirroredArray(
    std::array<uint8_t, N>& bytes,
    uint32_t base,
    uint32_t address,
    uint8_t value) {
    if (address < base) {
        return;
    }
    const uint32_t offset = address - base;
    bytes[offset % static_cast<uint32_t>(N)] = value;
}

} // namespace emugbds::ingba
