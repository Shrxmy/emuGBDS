#include "emugbds/ingba/Core.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace emugbds::ingba {
namespace {

constexpr uint32_t kBiosBase = 0x00000000u;
constexpr uint32_t kEwramBase = 0x02000000u;
constexpr uint32_t kIwramBase = 0x03000000u;
constexpr uint32_t kIoBase = 0x04000000u;
constexpr uint32_t kPaletteBase = 0x05000000u;
constexpr uint32_t kVramBase = 0x06000000u;
constexpr uint32_t kOamBase = 0x07000000u;
constexpr uint32_t kRomBase = 0x08000000u;

constexpr size_t kBiosSize = 0x00004000u;
constexpr size_t kEwramSize = 0x00040000u;
constexpr size_t kIwramSize = 0x00008000u;
constexpr size_t kIoSize = 0x00000400u;

constexpr uint32_t kCyclesPerFrame = 280896u;
constexpr uint32_t kCyclesPerScanline = 1232u;
constexpr uint32_t kVisibleScanlines = 160u;
constexpr uint32_t kTotalScanlines = 228u;
constexpr uint32_t kSwiVectorAddress = 0x00000008u;
constexpr uint32_t kKeyInputAddress = 0x04000130u;
constexpr uint32_t kDispCntAddress = 0x04000000u;
constexpr uint32_t kDispStatAddress = 0x04000004u;
constexpr uint32_t kVCountAddress = 0x04000006u;
constexpr uint32_t kBgCntAddressBase = 0x04000008u;
constexpr uint32_t kBgHofsAddressBase = 0x04000010u;
constexpr uint32_t kBgVofsAddressBase = 0x04000012u;
constexpr uint32_t kInterruptEnableAddress = 0x04000200u;
constexpr uint32_t kWaitCntAddress = 0x04000204u;
constexpr uint32_t kInterruptFlagAddress = 0x04000202u;
constexpr uint32_t kInterruptMasterEnableAddress = 0x04000208u;
constexpr uint32_t kIrqCallbackAddress = 0x03007FFCu;
constexpr uint32_t kPostFlgAddress = 0x04000300u;

constexpr uint32_t kModeMask = 0x0007u;
constexpr uint32_t kCpuModeMask = 0x1Fu;
constexpr uint32_t kCpuModeUser = 0x10u;
constexpr uint32_t kCpuModeFiq = 0x11u;
constexpr uint32_t kCpuModeIrq = 0x12u;
constexpr uint32_t kCpuModeSupervisor = 0x13u;
constexpr uint32_t kCpuModeAbort = 0x17u;
constexpr uint32_t kCpuModeUndefined = 0x1Bu;
constexpr uint32_t kCpuModeSystem = 0x1Fu;
constexpr uint32_t kCpsrThumbBit = 1u << 5;
constexpr uint32_t kCpsrIrqDisableBit = 1u << 7;
constexpr uint16_t kDispStatVBlank = 0x0001u;
constexpr uint16_t kDispStatHBlank = 0x0002u;
constexpr uint32_t kCompatGuardMagic = 0x3CC35AA5u;
constexpr uint32_t kCompatGuardAddrA = 0x02024C8Cu;
constexpr uint32_t kCompatGuardAddrB = 0x02024C90u;
constexpr std::array<uint32_t, 4u> kDmaBaseAddresses = {
    0x040000B0u,
    0x040000BCu,
    0x040000C8u,
    0x040000D4u,
};
constexpr std::array<uint32_t, 4u> kDmaControlHighByteAddresses = {
    0x040000BBu,
    0x040000C7u,
    0x040000D3u,
    0x040000DFu,
};
constexpr uint16_t kDmaEnableBit = 0x8000u;
constexpr uint16_t kDmaStartTimingMask = 0x3000u;
constexpr uint16_t kDmaTransfer32Bit = 0x0400u;
constexpr uint16_t kDmaRepeatBit = 0x0200u;

bool LoadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes, std::string& error) {
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

uint8_t ReadFromSpan(const std::vector<uint8_t>& bytes, uint32_t base, uint32_t address) {
    if (address < base) {
        return 0;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return 0;
    }
    return bytes[offset];
}

void WriteToSpan(std::vector<uint8_t>& bytes, uint32_t base, uint32_t address, uint8_t value) {
    if (address < base) {
        return;
    }
    const uint32_t offset = address - base;
    if (offset >= bytes.size()) {
        return;
    }
    bytes[offset] = value;
}

template <size_t N>
uint8_t ReadFromMirroredArray(const std::array<uint8_t, N>& bytes, uint32_t base, uint32_t address) {
    if (address < base) {
        return 0u;
    }
    const uint32_t offset = address - base;
    return bytes[offset % static_cast<uint32_t>(N)];
}

template <size_t N>
void WriteToMirroredArray(std::array<uint8_t, N>& bytes, uint32_t base, uint32_t address, uint8_t value) {
    if (address < base) {
        return;
    }
    const uint32_t offset = address - base;
    bytes[offset % static_cast<uint32_t>(N)] = value;
}

uint8_t Expand5To8(uint8_t value5) {
    return static_cast<uint8_t>((value5 << 3u) | (value5 >> 2u));
}

uint32_t Bgr555ToAbgr8888(uint16_t bgr555) {
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

std::string Hex32(uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

} // namespace

bool MemoryBus::LoadBios(const std::filesystem::path& bios_path, std::string& error) {
    std::vector<uint8_t> bios_image;
    if (!LoadWholeFile(bios_path, bios_image, error)) {
        return false;
    }
    if (bios_image.size() != kBiosSize) {
        error = "GBA BIOS must be exactly 16 KiB: " + bios_path.string();
        return false;
    }

    bios_ = std::move(bios_image);
    bios_loaded_ = true;
    if (ewram_.empty()) {
        ewram_.assign(kEwramSize, 0);
    }
    if (iwram_.empty()) {
        iwram_.assign(kIwramSize, 0);
    }
    return true;
}

bool MemoryBus::LoadRom(const std::filesystem::path& rom_path, std::string& error) {
    std::vector<uint8_t> rom_image;
    if (!LoadWholeFile(rom_path, rom_image, error)) {
        return false;
    }

    rom_ = std::move(rom_image);
    if (ewram_.empty()) {
        ewram_.assign(kEwramSize, 0);
    }
    if (iwram_.empty()) {
        iwram_.assign(kIwramSize, 0);
    }
    return true;
}

void MemoryBus::ApplyKeyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept {
    const uint16_t press = static_cast<uint16_t>(press_mask & 0x03FFu);
    const uint16_t release = static_cast<uint16_t>(release_mask & 0x03FFu);
    keyinput_state_ = static_cast<uint16_t>((keyinput_state_ & ~press) | release);
    keyinput_state_ &= 0x03FFu;
}

void MemoryBus::SetLcdStatus(uint16_t vcount, bool vblank, bool hblank) noexcept {
    const uint32_t dispstat_lo = kDispStatAddress - kIoBase;
    const uint32_t dispstat_hi = dispstat_lo + 1u;
    const uint32_t vcount_lo = kVCountAddress - kIoBase;
    const uint32_t vcount_hi = vcount_lo + 1u;

    uint16_t dispstat = static_cast<uint16_t>(io_[dispstat_lo] | (static_cast<uint16_t>(io_[dispstat_hi]) << 8u));
    dispstat &= static_cast<uint16_t>(~(kDispStatVBlank | kDispStatHBlank));
    if (vblank) {
        dispstat |= kDispStatVBlank;
    }
    if (hblank) {
        dispstat |= kDispStatHBlank;
    }

    io_[dispstat_lo] = static_cast<uint8_t>(dispstat & 0xFFu);
    io_[dispstat_hi] = static_cast<uint8_t>((dispstat >> 8u) & 0xFFu);
    io_[vcount_lo] = static_cast<uint8_t>(vcount & 0xFFu);
    io_[vcount_hi] = static_cast<uint8_t>((vcount >> 8u) & 0xFFu);
}

uint16_t MemoryBus::ReadIo16Direct(uint32_t address) const {
    if (address < kIoBase || address + 1u >= kIoBase + kIoSize) {
        return 0u;
    }
    const uint32_t offset = address - kIoBase;
    return static_cast<uint16_t>(io_[offset] | (static_cast<uint16_t>(io_[offset + 1u]) << 8u));
}

uint32_t MemoryBus::ReadIo32Direct(uint32_t address) const {
    if (address < kIoBase || address + 3u >= kIoBase + kIoSize) {
        return 0u;
    }
    const uint32_t offset = address - kIoBase;
    return static_cast<uint32_t>(io_[offset]) |
        (static_cast<uint32_t>(io_[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(io_[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(io_[offset + 3u]) << 24u);
}

void MemoryBus::WriteIo16Direct(uint32_t address, uint16_t value) {
    if (address < kIoBase || address + 1u >= kIoBase + kIoSize) {
        return;
    }
    const uint32_t offset = address - kIoBase;
    io_[offset] = static_cast<uint8_t>(value & 0xFFu);
    io_[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
}

void MemoryBus::WriteIo32Direct(uint32_t address, uint32_t value) {
    if (address < kIoBase || address + 3u >= kIoBase + kIoSize) {
        return;
    }
    const uint32_t offset = address - kIoBase;
    io_[offset] = static_cast<uint8_t>(value & 0xFFu);
    io_[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    io_[offset + 2u] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    io_[offset + 3u] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

void MemoryBus::TryStartImmediateDma(uint32_t control_high_address) {
    for (uint32_t channel = 0; channel < kDmaControlHighByteAddresses.size(); ++channel) {
        if (kDmaControlHighByteAddresses[channel] != control_high_address) {
            continue;
        }
        const uint32_t control_address = kDmaBaseAddresses[channel] + 10u;
        const uint16_t control = ReadIo16Direct(control_address);
        if ((control & kDmaEnableBit) == 0u) {
            return;
        }
        if ((control & kDmaStartTimingMask) != 0u) {
            return;
        }
        ExecuteDmaTransfer(channel, false);
        return;
    }
}

void MemoryBus::TriggerDmaTiming(uint32_t timing_code) {
    for (uint32_t channel = 0; channel < kDmaBaseAddresses.size(); ++channel) {
        const uint32_t control_address = kDmaBaseAddresses[channel] + 10u;
        const uint16_t control = ReadIo16Direct(control_address);
        if ((control & kDmaEnableBit) == 0u) {
            continue;
        }
        const uint32_t start_timing = (control >> 12u) & 0x3u;
        if (start_timing != timing_code) {
            continue;
        }
        ExecuteDmaTransfer(channel, true);
    }
}

void MemoryBus::SetInterruptRequest(uint16_t flags) noexcept {
    const uint16_t current = ReadIo16Direct(kInterruptFlagAddress);
    WriteIo16Direct(kInterruptFlagAddress, static_cast<uint16_t>(current | flags));
}

void MemoryBus::ExecuteDmaTransfer(uint32_t channel, bool timed_trigger) {
    if (channel >= kDmaBaseAddresses.size()) {
        return;
    }

    const uint32_t base = kDmaBaseAddresses[channel];
    const uint32_t initial_source = ReadIo32Direct(base + 0u);
    const uint32_t initial_destination = ReadIo32Direct(base + 4u);
    uint32_t source = initial_source;
    uint32_t destination = initial_destination;

    uint16_t count = ReadIo16Direct(base + 8u);
    uint16_t control = ReadIo16Direct(base + 10u);

    const bool word_transfer = (control & kDmaTransfer32Bit) != 0u;
    const uint32_t unit_size = word_transfer ? 4u : 2u;
    uint32_t units = count;
    if (units == 0u) {
        units = (channel == 3u) ? 0x10000u : 0x4000u;
    }

    const uint32_t destination_control = (control >> 5u) & 0x3u;
    const uint32_t source_control = (control >> 7u) & 0x3u;
    const bool destination_reload = destination_control == 0x3u;

    auto compute_delta = [&](uint32_t mode, bool source_mode) -> int32_t {
        switch (mode) {
        case 0x0u:
            return static_cast<int32_t>(unit_size);
        case 0x1u:
            return -static_cast<int32_t>(unit_size);
        case 0x2u:
            return 0;
        case 0x3u:
            if (source_mode) {
                return static_cast<int32_t>(unit_size);
            }
            return static_cast<int32_t>(unit_size);
        default:
            return static_cast<int32_t>(unit_size);
        }
    };

    const int32_t source_delta = compute_delta(source_control, true);
    const int32_t destination_delta = compute_delta(destination_control, false);

    for (uint32_t unit = 0u; unit < units; ++unit) {
        if (word_transfer) {
            const uint32_t value = Read32(source & ~3u);
            Write32(destination & ~3u, value);
        } else {
            const uint16_t value = Read16(source & ~1u);
            Write16(destination & ~1u, value);
        }

        source = static_cast<uint32_t>(static_cast<int64_t>(source) + source_delta);
        destination = static_cast<uint32_t>(static_cast<int64_t>(destination) + destination_delta);
    }

    WriteIo32Direct(base + 0u, source);
    if (destination_reload && (control & kDmaRepeatBit) != 0u) {
        WriteIo32Direct(base + 4u, initial_destination);
    } else {
        WriteIo32Direct(base + 4u, destination);
    }
    WriteIo16Direct(base + 8u, count);

    const bool keep_enabled = timed_trigger && (control & kDmaRepeatBit) != 0u &&
        ((control & kDmaStartTimingMask) != 0u);
    if (!keep_enabled) {
        control = static_cast<uint16_t>(control & ~kDmaEnableBit);
    }
    WriteIo16Direct(base + 10u, control);
}

uint8_t MemoryBus::Read8(uint32_t address) const {
    if (address >= kBiosBase && address < kBiosBase + kBiosSize) {
        if (!bios_loaded_) {
            return 0;
        }
        return ReadFromSpan(bios_, kBiosBase, address);
    }
    if (address >= kEwramBase && address < kEwramBase + kEwramSize) {
        return ReadFromSpan(ewram_, kEwramBase, address);
    }
    if (address >= kIwramBase && address < kIwramBase + kIwramSize) {
        return ReadFromSpan(iwram_, kIwramBase, address);
    }
    if (address >= kIoBase && address < kIoBase + kIoSize) {
        if (address == kKeyInputAddress) {
            return static_cast<uint8_t>(keyinput_state_ & 0xFFu);
        }
        if (address == kKeyInputAddress + 1u) {
            return static_cast<uint8_t>((keyinput_state_ >> 8u) & 0xFFu);
        }
        if (address == kPostFlgAddress) {
            return postflg_;
        }
        if (address == kPostFlgAddress + 1u) {
            return 0u;
        }
        if (address == kWaitCntAddress) {
            return static_cast<uint8_t>(waitcnt_ & 0xFFu);
        }
        if (address == kWaitCntAddress + 1u) {
            return static_cast<uint8_t>((waitcnt_ >> 8u) & 0xFFu);
        }
        return io_[address - kIoBase];
    }
    if (address >= kPaletteBase && address < kPaletteBase + 0x01000000u) {
        return ReadFromMirroredArray(palette_, kPaletteBase, address);
    }
    if (address >= kVramBase && address < kVramBase + 0x01000000u) {
        return ReadFromMirroredArray(vram_, kVramBase, address);
    }
    if (address >= kOamBase && address < kOamBase + 0x01000000u) {
        return ReadFromMirroredArray(oam_, kOamBase, address);
    }
    if (address == kKeyInputAddress) {
        return static_cast<uint8_t>(keyinput_state_ & 0xFFu);
    }
    if (address == kKeyInputAddress + 1u) {
        return static_cast<uint8_t>((keyinput_state_ >> 8u) & 0xFFu);
    }
    if (address >= kRomBase && !rom_.empty()) {
        const uint32_t rom_offset = (address - kRomBase) % static_cast<uint32_t>(rom_.size());
        return rom_[rom_offset];
    }
    return 0;
}

uint16_t MemoryBus::Read16(uint32_t address) const {
    const uint16_t lo = Read8(address);
    const uint16_t hi = Read8(address + 1);
    return static_cast<uint16_t>(lo | (hi << 8));
}

uint32_t MemoryBus::Read32(uint32_t address) const {
    const uint32_t b0 = Read8(address);
    const uint32_t b1 = Read8(address + 1);
    const uint32_t b2 = Read8(address + 2);
    const uint32_t b3 = Read8(address + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void MemoryBus::Write8(uint32_t address, uint8_t value) {
    if (address >= kEwramBase && address < kEwramBase + kEwramSize) {
        WriteToSpan(ewram_, kEwramBase, address, value);
        return;
    }
    if (address >= kIwramBase && address < kIwramBase + kIwramSize) {
        WriteToSpan(iwram_, kIwramBase, address, value);
        return;
    }
    if (address >= kIoBase && address < kIoBase + kIoSize) {
        if (address == kKeyInputAddress || address == kKeyInputAddress + 1u || address == kVCountAddress ||
            address == kVCountAddress + 1u || address == kPostFlgAddress + 1u) {
            return;
        }
        if (address == kInterruptFlagAddress || address == kInterruptFlagAddress + 1u) {
            const uint16_t current_if = ReadIo16Direct(kInterruptFlagAddress);
            const uint16_t clear_mask = (address == kInterruptFlagAddress)
                ? static_cast<uint16_t>(value)
                : static_cast<uint16_t>(static_cast<uint16_t>(value) << 8u);
            WriteIo16Direct(kInterruptFlagAddress, static_cast<uint16_t>(current_if & ~clear_mask));
            return;
        }

        if (address == kPostFlgAddress) {
            postflg_ = value;
            return;
        }
        if (address == kWaitCntAddress || address == kWaitCntAddress + 1u) {
            if (address == kWaitCntAddress) {
                waitcnt_ = static_cast<uint16_t>((waitcnt_ & 0xFF00u) | value);
            } else {
                waitcnt_ = static_cast<uint16_t>((waitcnt_ & 0x00FFu) | (static_cast<uint16_t>(value) << 8u));
            }
            io_[address - kIoBase] = value;
            return;
        }

        io_[address - kIoBase] = value;
        if (address == kDispCntAddress || address == kDispCntAddress + 1u) {
            ++dispcnt_write_count_;
            last_dispcnt_written_ = ReadIo16Direct(kDispCntAddress);
        }
        TryStartImmediateDma(address);
        return;
    }
    if (address >= kPaletteBase && address < kPaletteBase + 0x01000000u) {
        WriteToMirroredArray(palette_, kPaletteBase, address, value);
        return;
    }
    if (address >= kVramBase && address < kVramBase + 0x01000000u) {
        WriteToMirroredArray(vram_, kVramBase, address, value);
        return;
    }
    if (address >= kOamBase && address < kOamBase + 0x01000000u) {
        WriteToMirroredArray(oam_, kOamBase, address, value);
        return;
    }
}

void MemoryBus::Write16(uint32_t address, uint16_t value) {
    Write8(address, static_cast<uint8_t>(value & 0xFFu));
    Write8(address + 1, static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void MemoryBus::Write32(uint32_t address, uint32_t value) {
    Write8(address, static_cast<uint8_t>(value & 0xFFu));
    Write8(address + 1, static_cast<uint8_t>((value >> 8) & 0xFFu));
    Write8(address + 2, static_cast<uint8_t>((value >> 16) & 0xFFu));
    Write8(address + 3, static_cast<uint8_t>((value >> 24) & 0xFFu));
}

bool Core::Initialize(std::string& error) {
    (void)error;
    initialized_ = true;
    frame_counter_ = 0;
    unknown_arm_instruction_count_ = 0;
    unsupported_arm_stats_ = {};
    last_unsupported_swi_ = 0u;
    halted_ = false;
    return Reset(error);
}

bool Core::LoadBios(const std::filesystem::path& bios_path, std::string& error) {
    return bus_.LoadBios(bios_path, error);
}

bool Core::LoadRom(const std::filesystem::path& rom_path, std::string& error) {
    if (!bus_.LoadRom(rom_path, error)) {
        return false;
    }
    rom_loaded_ = true;
    if (!initialized_) {
        if (!Initialize(error)) {
            return false;
        }
    }
    return Reset(error);
}

bool Core::RunFrame(std::string& error) {
    if (!initialized_ || !rom_loaded_) {
        error = "In-house GBA core is not initialized with a ROM";
        return false;
    }

    uint32_t cycles = 0;
    AdvanceLcdTiming(lcd_cycle_in_frame_);
    while (cycles < kCyclesPerFrame) {
        // Temporary compatibility shim: avoid known guard-cookie lock loops until full
        // startup/interrupt runtime behavior is modeled.
        if (bus_.Read32(kCompatGuardAddrA) == kCompatGuardMagic) {
            bus_.Write32(kCompatGuardAddrA, 0x6A09E667u);
        }
        if (bus_.Read32(kCompatGuardAddrB) == kCompatGuardMagic) {
            bus_.Write32(kCompatGuardAddrB, 0xBB67AE85u);
        }
        if (halted_ && bus_.Read16(kInterruptFlagAddress) != 0u) {
            halted_ = false;
        }
        if (!halted_) {
            if (!StepArm(error)) {
                return false;
            }
        }
        ++cycles;
        lcd_cycle_in_frame_ = (lcd_cycle_in_frame_ + 1u) % kCyclesPerFrame;
        AdvanceLcdTiming(lcd_cycle_in_frame_);
    }

    UpdateVideoFrame();
    ++frame_counter_;
    return true;
}

bool Core::Reset(std::string& error) {
    (void)error;
    cpu_ = {};
    cpu_.cpsr = 0x000000D3u;
    cpu_.thumb = false;
    cpu_.r[15] = bus_.HasBios() ? kBiosBase : kRomBase;
    thumb_bl_prefix_pending_ = false;
    thumb_bl_prefix_base_ = 0;
    unknown_arm_instruction_count_ = 0;
    unsupported_arm_stats_ = {};
    last_unsupported_swi_ = 0u;
    halted_ = false;
    in_vblank_ = false;
    in_hblank_ = false;
    lcd_cycle_in_frame_ = 0;
    bus_.ApplyKeyInputMasks(0u, 0x03FFu);
    bus_.Write16(kWaitCntAddress, 0x4317u);
    bus_.Write8(kPostFlgAddress, 0x01u);
    bus_.SetLcdStatus(0u, false, false);
    video_buffer_.fill(0xFF000000u);
    return true;
}

void Core::ApplyInputMasks(uint32_t press_mask, uint32_t release_mask) noexcept {
    bus_.ApplyKeyInputMasks(press_mask, release_mask);
}

uint16_t Core::dispcnt() const noexcept {
    return bus_.Read16(kDispCntAddress);
}

uint16_t Core::dispstat() const noexcept {
    return bus_.Read16(kDispStatAddress);
}

uint16_t Core::vcount() const noexcept {
    return bus_.Read16(kVCountAddress);
}

uint16_t Core::bgcnt(uint32_t index) const noexcept {
    if (index > 3u) {
        return 0u;
    }
    return bus_.Read16(kBgCntAddressBase + (index * 2u));
}

uint16_t Core::iflags() const noexcept {
    return bus_.Read16(kInterruptFlagAddress);
}

uint16_t Core::ie() const noexcept {
    return bus_.Read16(kInterruptEnableAddress);
}

uint16_t Core::ime() const noexcept {
    return bus_.Read16(kInterruptMasterEnableAddress);
}

uint64_t Core::dispcnt_write_count() const noexcept {
    return bus_.dispcnt_write_count();
}

uint16_t Core::last_dispcnt_written() const noexcept {
    return bus_.last_dispcnt_written();
}

bool Core::StepArm(std::string& error) {
    ServicePendingInterrupts();

    if (cpu_.thumb) {
        const uint16_t instruction = bus_.Read16(cpu_.r[15]);
        const uint32_t pc = cpu_.r[15];
        const uint32_t fetch_pc = pc + 4u;
        return ExecuteThumbInstruction(instruction, pc, fetch_pc, error);
    }
    thumb_bl_prefix_pending_ = false;

    const uint32_t pc = cpu_.r[15];
    const uint32_t fetch_pc = pc + 8;
    const uint32_t instruction = bus_.Read32(pc);
    const uint32_t next_pc = pc + 4;

    if (!ConditionPassed(instruction)) {
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
        const uint32_t rn = instruction & 0xFu;
        const uint32_t target = ReadArmRegister(rn, fetch_pc);
        cpu_.thumb = (target & 1u) != 0;
        cpu_.r[15] = target & ~1u;
        return true;
    }

    if ((instruction & 0x0E000000u) == 0x0A000000u) {
        const int32_t signed_offset = static_cast<int32_t>(SignExtend24(instruction & 0x00FFFFFFu) << 2);
        if ((instruction & (1u << 24)) != 0u) {
            cpu_.r[14] = next_pc;
        }
        cpu_.r[15] = static_cast<uint32_t>(static_cast<int32_t>(fetch_pc) + signed_offset);
        return true;
    }

    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        if (ExecuteArmSoftwareInterrupt(instruction, next_pc, error)) {
            return true;
        }
        return HandleUnsupportedArm(UnsupportedArmClass::SoftwareInterrupt, next_pc, error);
    }

    const uint32_t top2 = (instruction >> 26) & 0x3u;
    if (top2 == 0x0u) {
        if ((instruction & 0x0F900FF0u) == 0x01000000u || // MRS
            (instruction & 0x0DB0F000u) == 0x0120F000u || // MSR register
            (instruction & 0x0DB0F000u) == 0x0320F000u) { // MSR immediate
            if (ExecuteArmPsrTransfer(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
        }
        if ((instruction & 0x0E000090u) == 0x00000090u && (instruction & 0x00000060u) != 0u) {
            if (ExecuteArmHalfwordTransfer(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
        }
        if ((instruction & 0x0F8000F0u) == 0x00800090u) {
            if (ExecuteArmMultiplyLong(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Multiply, next_pc, error);
        }
        if ((instruction & 0x0FC000F0u) == 0x00000090u) {
            if (ExecuteArmMultiply(instruction, fetch_pc, next_pc, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::Multiply, next_pc, error);
        } else if (ExecuteArmDataProcessing(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
    } else if (top2 == 0x1u) {
        if (ExecuteArmSingleDataTransfer(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
    } else if ((instruction & 0x0E000000u) == 0x08000000u) {
        if (ExecuteArmBlockDataTransfer(instruction, fetch_pc, next_pc, error)) {
            return true;
        }
        return HandleUnsupportedArm(UnsupportedArmClass::BlockTransfer, next_pc, error);
    }

    if (instruction == 0xE1A00000u || instruction == 0xE320F000u) {
        // NOP forms: MOV r0, r0 and YIELD as no-op.
        cpu_.r[15] = next_pc;
        return true;
    }

    return HandleUnsupportedArm(UnsupportedArmClass::Generic, next_pc, error);
}

bool Core::ExecuteThumbInstruction(uint16_t instruction, uint32_t pc, uint32_t fetch_pc, std::string& error) {
    const uint32_t next_pc = pc + 2u;

    auto logical_shift_left = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            carry_valid = false;
            carry_out = false;
            return value;
        }
        carry_valid = true;
        if (amount < 32u) {
            carry_out = ((value >> (32u - amount)) & 1u) != 0u;
            return value << amount;
        }
        if (amount == 32u) {
            carry_out = (value & 1u) != 0u;
            return 0u;
        }
        carry_out = false;
        return 0u;
    };

    auto logical_shift_right = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            amount = 32u;
        }
        carry_valid = true;
        if (amount < 32u) {
            carry_out = ((value >> (amount - 1u)) & 1u) != 0u;
            return value >> amount;
        }
        if (amount == 32u) {
            carry_out = ((value >> 31u) & 1u) != 0u;
            return 0u;
        }
        carry_out = false;
        return 0u;
    };

    auto arithmetic_shift_right = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            amount = 32u;
        }
        carry_valid = true;
        if (amount < 32u) {
            carry_out = ((value >> (amount - 1u)) & 1u) != 0u;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
        }
        const bool sign = (value & 0x80000000u) != 0u;
        carry_out = sign;
        return sign ? 0xFFFFFFFFu : 0u;
    };

    auto rotate_right = [&](uint32_t value, uint32_t amount, bool& carry_out, bool& carry_valid) -> uint32_t {
        if (amount == 0u) {
            carry_valid = false;
            carry_out = false;
            return value;
        }
        carry_valid = true;
        const uint32_t masked = amount & 31u;
        if (masked == 0u) {
            carry_out = ((value >> 31u) & 1u) != 0u;
            return value;
        }
        const uint32_t rotated = Ror(value, masked);
        carry_out = ((rotated >> 31u) & 1u) != 0u;
        return rotated;
    };

    if ((instruction & 0xF800u) == 0xF000u) {
        const int32_t upper = SignExtend(instruction & 0x07FFu, 11u) << 12u;
        thumb_bl_prefix_base_ = static_cast<uint32_t>(static_cast<int32_t>(fetch_pc) + upper);
        thumb_bl_prefix_pending_ = true;
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF800u) == 0xF800u) {
        if (!thumb_bl_prefix_pending_) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        const uint32_t lower = (instruction & 0x07FFu) << 1u;
        const uint32_t target = thumb_bl_prefix_base_ + lower;
        cpu_.r[14] = next_pc | 1u;
        cpu_.r[15] = target & ~1u;
        thumb_bl_prefix_pending_ = false;
        return true;
    }

    if (thumb_bl_prefix_pending_) {
        thumb_bl_prefix_pending_ = false;
    }

    if ((instruction & 0xF800u) == 0x1800u) {
        const bool immediate = (instruction & (1u << 10)) != 0u;
        const bool subtract = (instruction & (1u << 9)) != 0u;
        const uint32_t op2_index = (instruction >> 6) & 0x7u;
        const uint32_t rs = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t lhs = ReadThumbRegister(rs, fetch_pc);
        const uint32_t rhs = immediate ? op2_index : ReadThumbRegister(op2_index, fetch_pc);

        const AddResult add = subtract ? AddWithCarry(lhs, ~rhs, true) : AddWithCarry(lhs, rhs, false);
        WriteThumbRegister(rd, add.value);
        SetNZFlags(add.value);
        SetCFlag(add.carry);
        SetVFlag(add.overflow);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xE000u) == 0x0000u) {
        const uint32_t op = (instruction >> 11) & 0x3u;
        const uint32_t imm5 = (instruction >> 6) & 0x1Fu;
        const uint32_t rs = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t value = ReadThumbRegister(rs, fetch_pc);
        bool carry_out = false;
        bool carry_valid = false;
        uint32_t result = value;

        if (op == 0u) {
            result = logical_shift_left(value, imm5, carry_out, carry_valid);
        } else if (op == 1u) {
            result = logical_shift_right(value, imm5, carry_out, carry_valid);
        } else if (op == 2u) {
            result = arithmetic_shift_right(value, imm5, carry_out, carry_valid);
        } else {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }

        WriteThumbRegister(rd, result);
        SetNZFlags(result);
        if (carry_valid) {
            SetCFlag(carry_out);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xE000u) == 0x2000u) {
        const uint32_t op = (instruction >> 11) & 0x3u;
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t lhs = ReadThumbRegister(rd, fetch_pc);

        switch (op) {
        case 0x0: // MOV
            WriteThumbRegister(rd, imm8);
            SetNZFlags(imm8);
            break;
        case 0x1: { // CMP
            const AddResult add = AddWithCarry(lhs, ~imm8, true);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x2: { // ADD
            const AddResult add = AddWithCarry(lhs, imm8, false);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x3: { // SUB
            const AddResult add = AddWithCarry(lhs, ~imm8, true);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        default:
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xFC00u) == 0x4000u) {
        const uint32_t op = (instruction >> 6) & 0xFu;
        const uint32_t rs = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t rhs = ReadThumbRegister(rs, fetch_pc);
        const uint32_t lhs = ReadThumbRegister(rd, fetch_pc);
        uint32_t result = 0u;

        switch (op) {
        case 0x0: // AND
            result = lhs & rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0x1: // EOR
            result = lhs ^ rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0x2: { // LSL
            bool carry_out = false;
            bool carry_valid = false;
            result = logical_shift_left(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x3: { // LSR
            bool carry_out = false;
            bool carry_valid = false;
            result = logical_shift_right(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x4: { // ASR
            bool carry_out = false;
            bool carry_valid = false;
            result = arithmetic_shift_right(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x5: { // ADC
            const AddResult add = AddWithCarry(lhs, rhs, (cpu_.cpsr & (1u << 29)) != 0u);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x6: { // SBC
            const AddResult add = AddWithCarry(lhs, ~rhs, (cpu_.cpsr & (1u << 29)) != 0u);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0x7: { // ROR
            bool carry_out = false;
            bool carry_valid = false;
            result = rotate_right(lhs, rhs & 0xFFu, carry_out, carry_valid);
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            if (carry_valid) {
                SetCFlag(carry_out);
            }
            break;
        }
        case 0x8: // TST
            result = lhs & rhs;
            SetNZFlags(result);
            break;
        case 0x9: { // NEG
            const AddResult add = AddWithCarry(0u, ~rhs, true);
            WriteThumbRegister(rd, add.value);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0xA: { // CMP
            const AddResult add = AddWithCarry(lhs, ~rhs, true);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0xB: { // CMN
            const AddResult add = AddWithCarry(lhs, rhs, false);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            break;
        }
        case 0xC: // ORR
            result = lhs | rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0xD: // MUL
            result = lhs * rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0xE: // BIC
            result = lhs & ~rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        case 0xF: // MVN
            result = ~rhs;
            WriteThumbRegister(rd, result);
            SetNZFlags(result);
            break;
        default:
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xFC00u) == 0x4400u) {
        const uint32_t op = (instruction >> 8) & 0x3u;
        const uint32_t rd = ((instruction >> 4) & 0x8u) | (instruction & 0x7u);
        const uint32_t rs = ((instruction >> 3) & 0x8u) | ((instruction >> 3) & 0x7u);
        const uint32_t lhs = ReadThumbRegister(rd, fetch_pc);
        const uint32_t rhs = ReadThumbRegister(rs, fetch_pc);

        switch (op) {
        case 0x0: { // ADD
            const uint32_t value = lhs + rhs;
            WriteThumbRegister(rd, value);
            if (rd != 15u) {
                cpu_.r[15] = next_pc;
            }
            return true;
        }
        case 0x1: { // CMP
            const AddResult add = AddWithCarry(lhs, ~rhs, true);
            SetNZFlags(add.value);
            SetCFlag(add.carry);
            SetVFlag(add.overflow);
            cpu_.r[15] = next_pc;
            return true;
        }
        case 0x2: // MOV
            WriteThumbRegister(rd, rhs);
            if (rd != 15u) {
                cpu_.r[15] = next_pc;
            }
            return true;
        case 0x3: { // BX
            cpu_.thumb = (rhs & 1u) != 0u;
            cpu_.r[15] = rhs & ~1u;
            thumb_bl_prefix_pending_ = false;
            return true;
        }
        default:
            break;
        }
        return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
    }

    if ((instruction & 0xF800u) == 0x4800u) {
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t address = (fetch_pc & ~3u) + (imm8 << 2u);
        const uint32_t value = bus_.Read32(address & ~3u);
        WriteThumbRegister(rd, value);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0x5000u) {
        const uint32_t rm = (instruction >> 6) & 0x7u;
        const uint32_t rn = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t address = ReadThumbRegister(rn, fetch_pc) + ReadThumbRegister(rm, fetch_pc);

        if ((instruction & (1u << 9)) == 0u) {
            const bool load = (instruction & (1u << 11)) != 0u;
            const bool byte_transfer = (instruction & (1u << 10)) != 0u;
            if (load) {
                const uint32_t value = byte_transfer ? bus_.Read8(address) : bus_.Read32(address & ~3u);
                WriteThumbRegister(rd, value);
            } else {
                const uint32_t value = ReadThumbRegister(rd, fetch_pc);
                if (byte_transfer) {
                    bus_.Write8(address, static_cast<uint8_t>(value & 0xFFu));
                } else {
                    bus_.Write32(address & ~3u, value);
                }
            }
            cpu_.r[15] = next_pc;
            return true;
        }

        const uint32_t op = (instruction >> 10) & 0x3u;
        switch (op) {
        case 0x0: { // STRH
            const uint16_t value = static_cast<uint16_t>(ReadThumbRegister(rd, fetch_pc) & 0xFFFFu);
            bus_.Write16(address & ~1u, value);
            break;
        }
        case 0x1: { // LDSB
            const int8_t value = static_cast<int8_t>(bus_.Read8(address));
            WriteThumbRegister(rd, static_cast<uint32_t>(static_cast<int32_t>(value)));
            break;
        }
        case 0x2: { // LDRH
            const uint32_t value = bus_.Read16(address & ~1u);
            WriteThumbRegister(rd, value);
            break;
        }
        case 0x3: { // LDSH
            const int16_t value = static_cast<int16_t>(bus_.Read16(address & ~1u));
            WriteThumbRegister(rd, static_cast<uint32_t>(static_cast<int32_t>(value)));
            break;
        }
        default:
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xE000u) == 0x6000u) {
        const bool byte_transfer = (instruction & (1u << 12)) != 0u;
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t imm5 = (instruction >> 6) & 0x1Fu;
        const uint32_t rn = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t offset = byte_transfer ? imm5 : (imm5 << 2u);
        const uint32_t address = ReadThumbRegister(rn, fetch_pc) + offset;

        if (load) {
            const uint32_t value = byte_transfer ? bus_.Read8(address) : bus_.Read32(address & ~3u);
            WriteThumbRegister(rd, value);
        } else {
            const uint32_t value = ReadThumbRegister(rd, fetch_pc);
            if (byte_transfer) {
                bus_.Write8(address, static_cast<uint8_t>(value & 0xFFu));
            } else {
                bus_.Write32(address & ~3u, value);
            }
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0x8000u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t imm5 = (instruction >> 6) & 0x1Fu;
        const uint32_t rn = (instruction >> 3) & 0x7u;
        const uint32_t rd = instruction & 0x7u;
        const uint32_t address = ReadThumbRegister(rn, fetch_pc) + (imm5 << 1u);

        if (load) {
            const uint32_t value = bus_.Read16(address & ~1u);
            WriteThumbRegister(rd, value);
        } else {
            const uint16_t value = static_cast<uint16_t>(ReadThumbRegister(rd, fetch_pc) & 0xFFFFu);
            bus_.Write16(address & ~1u, value);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0x9000u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t address = cpu_.r[13] + (imm8 << 2u);
        if (load) {
            WriteThumbRegister(rd, bus_.Read32(address & ~3u));
        } else {
            bus_.Write32(address & ~3u, ReadThumbRegister(rd, fetch_pc));
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0xA000u) {
        const bool use_sp = (instruction & (1u << 11)) != 0u;
        const uint32_t rd = (instruction >> 8) & 0x7u;
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t base = use_sp ? cpu_.r[13] : (fetch_pc & ~3u);
        WriteThumbRegister(rd, base + (imm8 << 2u));
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xFF00u) == 0xB000u) {
        const bool subtract = (instruction & (1u << 7)) != 0u;
        const uint32_t offset = (instruction & 0x7Fu) << 2u;
        cpu_.r[13] = subtract ? (cpu_.r[13] - offset) : (cpu_.r[13] + offset);
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF600u) == 0xB400u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const bool include_extra = (instruction & (1u << 8)) != 0u;
        uint32_t reg_list = instruction & 0xFFu;
        if (load && include_extra) {
            reg_list |= (1u << 15);
        } else if (!load && include_extra) {
            reg_list |= (1u << 14);
        }
        if (reg_list == 0u) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }

        uint32_t count = 0u;
        for (uint32_t reg = 0; reg < 16u; ++reg) {
            if ((reg_list & (1u << reg)) != 0u) {
                ++count;
            }
        }

        if (load) {
            uint32_t address = cpu_.r[13];
            bool loaded_pc = false;
            for (uint32_t reg = 0; reg < 16u; ++reg) {
                if ((reg_list & (1u << reg)) == 0u) {
                    continue;
                }
                const uint32_t value = bus_.Read32(address & ~3u);
                if (reg == 15u) {
                    cpu_.thumb = (value & 1u) != 0u;
                    cpu_.r[15] = value & ~1u;
                    loaded_pc = true;
                } else {
                    cpu_.r[reg] = value;
                }
                address += 4u;
            }
            cpu_.r[13] += count * 4u;
            if (!loaded_pc) {
                cpu_.r[15] = next_pc;
            }
        } else {
            uint32_t address = cpu_.r[13] - (count * 4u);
            for (uint32_t reg = 0; reg < 16u; ++reg) {
                if ((reg_list & (1u << reg)) == 0u) {
                    continue;
                }
                bus_.Write32(address & ~3u, cpu_.r[reg]);
                address += 4u;
            }
            cpu_.r[13] -= count * 4u;
            cpu_.r[15] = next_pc;
        }
        return true;
    }

    if ((instruction & 0xF000u) == 0xC000u) {
        const bool load = (instruction & (1u << 11)) != 0u;
        const uint32_t rn = (instruction >> 8) & 0x7u;
        const uint32_t reg_list = instruction & 0xFFu;
        if (reg_list == 0u) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }

        uint32_t count = 0u;
        for (uint32_t reg = 0; reg < 8u; ++reg) {
            if ((reg_list & (1u << reg)) != 0u) {
                ++count;
            }
        }

        uint32_t address = cpu_.r[rn];
        for (uint32_t reg = 0; reg < 8u; ++reg) {
            if ((reg_list & (1u << reg)) == 0u) {
                continue;
            }
            if (load) {
                cpu_.r[reg] = bus_.Read32(address & ~3u);
            } else {
                bus_.Write32(address & ~3u, cpu_.r[reg]);
            }
            address += 4u;
        }
        cpu_.r[rn] += count * 4u;
        cpu_.r[15] = next_pc;
        return true;
    }

    if ((instruction & 0xF000u) == 0xD000u) {
        const uint32_t cond = (instruction >> 8) & 0xFu;
        const uint8_t imm8 = static_cast<uint8_t>(instruction & 0xFFu);
        if (cond == 0xFu) {
            if (ExecuteSoftwareInterrupt(imm8, next_pc, true, error)) {
                return true;
            }
            return HandleUnsupportedArm(UnsupportedArmClass::SoftwareInterrupt, next_pc, error);
        }
        if (cond == 0xEu) {
            return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
        }
        if (ConditionPassed(cond << 28u)) {
            const int32_t offset = SignExtend(imm8, 8u) << 1u;
            cpu_.r[15] = static_cast<uint32_t>(static_cast<int32_t>(fetch_pc) + offset);
        } else {
            cpu_.r[15] = next_pc;
        }
        return true;
    }

    if ((instruction & 0xF800u) == 0xE000u) {
        const int32_t offset = SignExtend(instruction & 0x07FFu, 11u) << 1u;
        cpu_.r[15] = static_cast<uint32_t>(static_cast<int32_t>(fetch_pc) + offset);
        return true;
    }

    return HandleUnsupportedArm(UnsupportedArmClass::Thumb, next_pc, error);
}

bool Core::ExecuteArmDataProcessing(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const uint32_t opcode = (instruction >> 21) & 0xFu;
    const bool set_flags = (instruction & (1u << 20)) != 0;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t rd = (instruction >> 12) & 0xFu;

    uint32_t operand2 = 0;
    bool shifter_carry = (cpu_.cpsr & (1u << 29)) != 0;
    if (!DecodeArmOperand2(instruction, fetch_pc, operand2, shifter_carry)) {
        return false;
    }

    const uint32_t operand1 = ReadArmRegister(rn, fetch_pc);

    uint32_t result = 0;
    bool write_result = true;
    bool use_logic_flags = false;
    bool carry_out = false;
    bool overflow_out = false;

    switch (opcode) {
    case 0x0: // AND
        result = operand1 & operand2;
        use_logic_flags = true;
        break;
    case 0x1: // EOR
        result = operand1 ^ operand2;
        use_logic_flags = true;
        break;
    case 0x2: { // SUB
        const AddResult add = AddWithCarry(operand1, ~operand2, true);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x3: { // RSB
        const AddResult add = AddWithCarry(operand2, ~operand1, true);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x4: { // ADD
        const AddResult add = AddWithCarry(operand1, operand2, false);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x5: { // ADC
        const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;
        const AddResult add = AddWithCarry(operand1, operand2, carry_in);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x6: { // SBC
        const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;
        const AddResult add = AddWithCarry(operand1, ~operand2, carry_in);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x7: { // RSC
        const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;
        const AddResult add = AddWithCarry(operand2, ~operand1, carry_in);
        result = add.value;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0x8: // TST
        result = operand1 & operand2;
        write_result = false;
        use_logic_flags = true;
        break;
    case 0x9: // TEQ
        result = operand1 ^ operand2;
        write_result = false;
        use_logic_flags = true;
        break;
    case 0xA: { // CMP
        const AddResult add = AddWithCarry(operand1, ~operand2, true);
        result = add.value;
        write_result = false;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0xB: { // CMN
        const AddResult add = AddWithCarry(operand1, operand2, false);
        result = add.value;
        write_result = false;
        carry_out = add.carry;
        overflow_out = add.overflow;
        break;
    }
    case 0xC: // ORR
        result = operand1 | operand2;
        use_logic_flags = true;
        break;
    case 0xD: // MOV
        result = operand2;
        use_logic_flags = true;
        break;
    case 0xE: // BIC
        result = operand1 & ~operand2;
        use_logic_flags = true;
        break;
    case 0xF: // MVN
        result = ~operand2;
        use_logic_flags = true;
        break;
    default:
        return false;
    }

    if (set_flags) {
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | ((result & 0x80000000u) ? (1u << 31) : 0u);
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | ((result == 0) ? (1u << 30) : 0u);

        if (use_logic_flags) {
            cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (shifter_carry ? (1u << 29) : 0u);
        } else {
            cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (carry_out ? (1u << 29) : 0u);
            cpu_.cpsr = (cpu_.cpsr & ~(1u << 28)) | (overflow_out ? (1u << 28) : 0u);
        }
    }

    if (write_result) {
        WriteArmRegister(rd, result);
        if (rd == 15u) {
            if (set_flags && ModeHasSpsr(CurrentMode())) {
                WriteCpsr(ReadCurrentSpsr());
            }
            return true;
        }
    }

    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmPsrTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool is_mrs = (instruction & 0x0FBF0FFFu) == 0x010F0000u;
    if (is_mrs) {
        const bool use_spsr = (instruction & (1u << 22)) != 0u;
        const uint32_t rd = (instruction >> 12u) & 0xFu;
        if (use_spsr) {
            if (!ModeHasSpsr(CurrentMode())) {
                return false;
            }
            WriteArmRegister(rd, ReadCurrentSpsr());
        } else {
            WriteArmRegister(rd, cpu_.cpsr);
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    const bool is_msr_reg = (instruction & 0x0DB0F010u) == 0x0120F000u;
    const bool is_msr_imm = (instruction & 0x0DB0F000u) == 0x0320F000u;
    if (!is_msr_reg && !is_msr_imm) {
        return false;
    }

    const bool use_spsr = (instruction & (1u << 22)) != 0u;
    uint32_t value = 0u;
    if (is_msr_reg) {
        const uint32_t rm = instruction & 0xFu;
        value = ReadArmRegister(rm, fetch_pc);
    } else {
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t rotate = ((instruction >> 8u) & 0xFu) * 2u;
        value = Ror(imm8, rotate);
    }

    const uint32_t field_mask = (instruction >> 16u) & 0xFu;
    uint32_t byte_mask = 0u;
    if ((field_mask & 0x8u) != 0u) {
        byte_mask |= 0xFF000000u;
    }
    if ((field_mask & 0x4u) != 0u) {
        byte_mask |= 0x00FF0000u;
    }
    if ((field_mask & 0x2u) != 0u) {
        byte_mask |= 0x0000FF00u;
    }
    if ((field_mask & 0x1u) != 0u) {
        byte_mask |= 0x000000FFu;
    }
    if (byte_mask == 0u) {
        byte_mask = 0xF0000000u;
    }

    if (!CurrentModeIsPrivileged()) {
        byte_mask &= 0xFFFFFF00u;
    }

    if (use_spsr) {
        if (!ModeHasSpsr(CurrentMode())) {
            return false;
        }
        const uint32_t merged_spsr = (ReadCurrentSpsr() & ~byte_mask) | (value & byte_mask);
        if (!WriteCurrentSpsr(merged_spsr)) {
            return false;
        }
        cpu_.r[15] = next_pc;
        return true;
    }

    const uint32_t merged_cpsr = (cpu_.cpsr & ~byte_mask) | (value & byte_mask);
    WriteCpsr(merged_cpsr);
    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmSingleDataTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool immediate_offset = (instruction & (1u << 25)) == 0;
    const bool pre_index = (instruction & (1u << 24)) != 0;
    const bool add_offset = (instruction & (1u << 23)) != 0;
    const bool byte_transfer = (instruction & (1u << 22)) != 0;
    const bool write_back = (instruction & (1u << 21)) != 0;
    const bool load = (instruction & (1u << 20)) != 0;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t rd = (instruction >> 12) & 0xFu;

    uint32_t offset = 0;
    if (immediate_offset) {
        offset = instruction & 0xFFFu;
    } else {
        bool discard_carry = false;
        offset = DecodeShiftedRegister(instruction, fetch_pc, discard_carry);
    }

    const uint32_t base = ReadArmRegister(rn, fetch_pc);
    uint32_t address = base;
    uint32_t updated_base = base;

    if (pre_index) {
        address = add_offset ? (base + offset) : (base - offset);
        updated_base = address;
    } else {
        updated_base = add_offset ? (base + offset) : (base - offset);
    }

    if (load) {
        uint32_t value = 0;
        if (byte_transfer) {
            value = bus_.Read8(address);
        } else {
            value = bus_.Read32(address & ~3u);
        }

        WriteArmRegister(rd, value);
        if (rd == 15u) {
            if (!pre_index || write_back) {
                WriteArmRegister(rn, updated_base);
            }
            return true;
        }
    } else {
        uint32_t value = ReadArmRegister(rd, fetch_pc);
        if (byte_transfer) {
            bus_.Write8(address, static_cast<uint8_t>(value & 0xFFu));
        } else {
            bus_.Write32(address & ~3u, value);
        }
    }

    if (!pre_index || write_back) {
        WriteArmRegister(rn, updated_base);
    }

    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmHalfwordTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool pre_index = (instruction & (1u << 24)) != 0u;
    const bool add_offset = (instruction & (1u << 23)) != 0u;
    const bool immediate_offset = (instruction & (1u << 22)) != 0u;
    const bool write_back = (instruction & (1u << 21)) != 0u;
    const bool load = (instruction & (1u << 20)) != 0u;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t rd = (instruction >> 12) & 0xFu;
    const uint32_t op = (instruction >> 5) & 0x3u; // 01=H,10=SB,11=SH

    uint32_t offset = 0u;
    if (immediate_offset) {
        offset = ((instruction >> 8u) & 0xFu) << 4u;
        offset |= (instruction & 0xFu);
    } else {
        const uint32_t rm = instruction & 0xFu;
        offset = ReadArmRegister(rm, fetch_pc);
    }

    const uint32_t base = ReadArmRegister(rn, fetch_pc);
    uint32_t address = base;
    uint32_t updated_base = base;

    if (pre_index) {
        address = add_offset ? (base + offset) : (base - offset);
        updated_base = address;
    } else {
        updated_base = add_offset ? (base + offset) : (base - offset);
    }

    if (!load) {
        if (op != 0x1u) {
            return false;
        }
        const uint16_t value = static_cast<uint16_t>(ReadArmRegister(rd, fetch_pc) & 0xFFFFu);
        bus_.Write16(address & ~1u, value);
    } else {
        uint32_t value = 0u;
        switch (op) {
        case 0x1u: // LDRH
            value = bus_.Read16(address & ~1u);
            break;
        case 0x2u: { // LDRSB
            const int8_t signed_byte = static_cast<int8_t>(bus_.Read8(address));
            value = static_cast<uint32_t>(static_cast<int32_t>(signed_byte));
            break;
        }
        case 0x3u: { // LDRSH
            const int16_t signed_half = static_cast<int16_t>(bus_.Read16(address & ~1u));
            value = static_cast<uint32_t>(static_cast<int32_t>(signed_half));
            break;
        }
        default:
            return false;
        }
        WriteArmRegister(rd, value);
        if (rd == 15u) {
            if (!pre_index || write_back) {
                WriteArmRegister(rn, updated_base);
            }
            return true;
        }
    }

    if (!pre_index || write_back) {
        WriteArmRegister(rn, updated_base);
    }
    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmMultiply(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool accumulate = (instruction & (1u << 21)) != 0u;
    const bool set_flags = (instruction & (1u << 20)) != 0u;

    const uint32_t rd = (instruction >> 16) & 0xFu;
    const uint32_t rn = (instruction >> 12) & 0xFu;
    const uint32_t rs = (instruction >> 8) & 0xFu;
    const uint32_t rm = instruction & 0xFu;

    if (rd == 15u) {
        return false;
    }

    const uint64_t lhs = static_cast<uint64_t>(ReadArmRegister(rm, fetch_pc));
    const uint64_t rhs = static_cast<uint64_t>(ReadArmRegister(rs, fetch_pc));
    uint64_t wide = lhs * rhs;

    if (accumulate) {
        wide += static_cast<uint64_t>(ReadArmRegister(rn, fetch_pc));
    }

    const uint32_t result = static_cast<uint32_t>(wide & 0xFFFFFFFFu);
    WriteArmRegister(rd, result);

    if (set_flags) {
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | ((result & 0x80000000u) ? (1u << 31) : 0u);
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | ((result == 0u) ? (1u << 30) : 0u);
    }

    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmMultiplyLong(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool signed_multiply = (instruction & (1u << 22)) != 0u;
    const bool accumulate = (instruction & (1u << 21)) != 0u;
    const bool set_flags = (instruction & (1u << 20)) != 0u;

    const uint32_t rd_hi = (instruction >> 16) & 0xFu;
    const uint32_t rd_lo = (instruction >> 12) & 0xFu;
    const uint32_t rs = (instruction >> 8) & 0xFu;
    const uint32_t rm = instruction & 0xFu;

    if (rd_hi == 15u || rd_lo == 15u || rd_hi == rd_lo) {
        return false;
    }

    uint64_t wide_result = 0;
    if (signed_multiply) {
        const int64_t lhs = static_cast<int64_t>(static_cast<int32_t>(ReadArmRegister(rm, fetch_pc)));
        const int64_t rhs = static_cast<int64_t>(static_cast<int32_t>(ReadArmRegister(rs, fetch_pc)));
        wide_result = static_cast<uint64_t>(lhs * rhs);
    } else {
        const uint64_t lhs = static_cast<uint64_t>(ReadArmRegister(rm, fetch_pc));
        const uint64_t rhs = static_cast<uint64_t>(ReadArmRegister(rs, fetch_pc));
        wide_result = lhs * rhs;
    }

    if (accumulate) {
        const uint64_t current =
            (static_cast<uint64_t>(cpu_.r[rd_hi]) << 32u) | static_cast<uint64_t>(cpu_.r[rd_lo]);
        wide_result += current;
    }

    cpu_.r[rd_lo] = static_cast<uint32_t>(wide_result & 0xFFFFFFFFu);
    cpu_.r[rd_hi] = static_cast<uint32_t>((wide_result >> 32u) & 0xFFFFFFFFu);

    if (set_flags) {
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | ((wide_result & (1ull << 63u)) ? (1u << 31) : 0u);
        cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | ((wide_result == 0u) ? (1u << 30) : 0u);
    }

    cpu_.r[15] = next_pc;
    return true;
}

bool Core::ExecuteArmBlockDataTransfer(
    uint32_t instruction, uint32_t fetch_pc, uint32_t next_pc, std::string& error) {
    (void)error;
    const bool pre_index = (instruction & (1u << 24)) != 0u;
    const bool add_offset = (instruction & (1u << 23)) != 0u;
    const bool psr_and_force_user = (instruction & (1u << 22)) != 0u;
    const bool write_back = (instruction & (1u << 21)) != 0u;
    const bool load = (instruction & (1u << 20)) != 0u;
    const uint32_t rn = (instruction >> 16) & 0xFu;
    const uint32_t reg_list = instruction & 0xFFFFu;

    if (reg_list == 0u) {
        return false;
    }

    uint32_t reg_count = 0;
    for (uint32_t reg = 0; reg < 16u; ++reg) {
        if ((reg_list & (1u << reg)) != 0u) {
            ++reg_count;
        }
    }

    const uint32_t base = ReadArmRegister(rn, fetch_pc);
    uint32_t start_address = base;
    uint32_t updated_base = base;

    if (add_offset) {
        start_address = pre_index ? (base + 4u) : base;
        updated_base = base + (4u * reg_count);
    } else {
        start_address = pre_index ? (base - (4u * reg_count)) : (base - (4u * (reg_count - 1u)));
        updated_base = base - (4u * reg_count);
    }

    uint32_t address = start_address;
    bool loaded_pc = false;
    for (uint32_t reg = 0; reg < 16u; ++reg) {
        if ((reg_list & (1u << reg)) == 0u) {
            continue;
        }

        if (load) {
            const uint32_t value = bus_.Read32(address & ~3u);
            WriteArmRegister(reg, value);
            if (reg == 15u) {
                loaded_pc = true;
            }
        } else {
            const uint32_t value = ReadArmRegister(reg, fetch_pc);
            bus_.Write32(address & ~3u, value);
        }

        address += 4u;
    }

    if (write_back) {
        WriteArmRegister(rn, updated_base);
    }

    if (loaded_pc && load && psr_and_force_user && ModeHasSpsr(CurrentMode())) {
        WriteCpsr(ReadCurrentSpsr());
    }

    if (!loaded_pc) {
        cpu_.r[15] = next_pc;
    }
    return true;
}

bool Core::ExecuteArmSoftwareInterrupt(uint32_t instruction, uint32_t next_pc, std::string& error) {
    const uint8_t swi_imm = static_cast<uint8_t>(instruction & 0xFFu);
    return ExecuteSoftwareInterrupt(swi_imm, next_pc, false, error);
}

bool Core::ExecuteSoftwareInterrupt(uint8_t swi_imm, uint32_t next_pc, bool from_thumb, std::string& error) {
    const uint32_t saved_cpsr = cpu_.cpsr;
    WriteSpsrForMode(kCpuModeSupervisor, saved_cpsr);
    uint32_t svc_cpsr = (saved_cpsr & ~kCpuModeMask) | kCpuModeSupervisor;
    svc_cpsr |= kCpsrIrqDisableBit;
    svc_cpsr &= ~kCpsrThumbBit;
    WriteCpsr(svc_cpsr);
    cpu_.r[14] = next_pc | (from_thumb ? 1u : 0u);

    const bool handled = HandleSoftwareInterrupt(swi_imm, error);

    WriteCpsr(saved_cpsr);
    cpu_.thumb = from_thumb;
    cpu_.r[15] = next_pc & (from_thumb ? ~1u : ~3u);
    return handled;
}

bool Core::HandleSoftwareInterrupt(uint8_t swi_imm, std::string& error) {
    switch (swi_imm) {
    case 0x00: // SoftReset
        if (!Reset(error)) {
            return false;
        }
        cpu_.r[15] = rom_loaded_ ? kRomBase : kBiosBase;
        return true;
    case 0x01: { // RegisterRamReset
        const uint32_t flags = cpu_.r[0] & 0xFFu;
        if ((flags & 0x01u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x40000u; offset += 4u) {
                bus_.Write32(kEwramBase + offset, 0u);
            }
        }
        if ((flags & 0x02u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x8000u; offset += 4u) {
                bus_.Write32(kIwramBase + offset, 0u);
            }
        }
        if ((flags & 0x04u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x400u; offset += 4u) {
                bus_.Write32(kPaletteBase + offset, 0u);
            }
        }
        if ((flags & 0x08u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x18000u; offset += 4u) {
                bus_.Write32(kVramBase + offset, 0u);
            }
        }
        if ((flags & 0x10u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x400u; offset += 4u) {
                bus_.Write32(kOamBase + offset, 0u);
            }
        }
        if ((flags & 0x20u) != 0u || (flags & 0x40u) != 0u || (flags & 0x80u) != 0u) {
            for (uint32_t offset = 0u; offset < 0x400u; offset += 4u) {
                bus_.Write32(kIoBase + offset, 0u);
            }
        }
        return true;
    }
    case 0x02: // Halt
    case 0x03: // Stop/Sleep
        halted_ = true;
    case 0x19: // SoundBias
    case 0x2A: // SoundDriverJmpTableCopy
        return true;
    case 0x0D: // GetBiosChecksum
        cpu_.r[0] = 0xBAAE187Fu;
        return true;
    case 0x0E: // BgAffineSet
        return ExecuteBgAffineSet();
    case 0x04: // IntrWait
        return ExecuteIntrWait(false, error);
    case 0x05: // VBlankIntrWait
        return ExecuteIntrWait(true, error);
    case 0x06: { // Div
        const int64_t numerator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[0]));
        const int64_t denominator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[1]));
        if (denominator == 0) {
            cpu_.r[0] = 0u;
            cpu_.r[1] = static_cast<uint32_t>(numerator);
            cpu_.r[3] = 0u;
            return true;
        }
        const int64_t quotient = numerator / denominator;
        const int64_t remainder = numerator % denominator;
        cpu_.r[0] = static_cast<uint32_t>(quotient);
        cpu_.r[1] = static_cast<uint32_t>(remainder);
        const int64_t abs_quotient = quotient < 0 ? -quotient : quotient;
        cpu_.r[3] = static_cast<uint32_t>(abs_quotient);
        return true;
    }
    case 0x07: { // DivArm (r0=denominator, r1=numerator)
        const int64_t denominator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[0]));
        const int64_t numerator = static_cast<int64_t>(static_cast<int32_t>(cpu_.r[1]));
        if (denominator == 0) {
            cpu_.r[0] = 0u;
            cpu_.r[1] = static_cast<uint32_t>(numerator);
            cpu_.r[3] = 0u;
            return true;
        }
        const int64_t quotient = numerator / denominator;
        const int64_t remainder = numerator % denominator;
        cpu_.r[0] = static_cast<uint32_t>(quotient);
        cpu_.r[1] = static_cast<uint32_t>(remainder);
        const int64_t abs_quotient = quotient < 0 ? -quotient : quotient;
        cpu_.r[3] = static_cast<uint32_t>(abs_quotient);
        return true;
    }
    case 0x08: { // Sqrt
        const double root = std::sqrt(static_cast<double>(cpu_.r[0]));
        cpu_.r[0] = static_cast<uint32_t>(root);
        return true;
    }
    case 0x0B: // CpuSet
        return ExecuteCpuSet(false);
    case 0x0C: // CpuFastSet
        return ExecuteCpuSet(true);
    case 0x11: // LZ77UnCompWram
        return ExecuteLz77UnComp(false, error);
    case 0x12: // LZ77UnCompVram
        return ExecuteLz77UnComp(true, error);
    case 0x14: // RLUnCompWram
        return ExecuteRlUnComp(false, error);
    case 0x15: // RLUnCompVram
        return ExecuteRlUnComp(true, error);
    default:
        last_unsupported_swi_ = swi_imm;
        error = "Unsupported GBA BIOS SWI: " + std::to_string(static_cast<unsigned>(swi_imm));
        return false;
    }
}

bool Core::ExecuteCpuSet(bool fast) {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t control = cpu_.r[2];
    const bool fill = (control & (1u << 24)) != 0u;

    if (fast) {
        const uint32_t units = control & 0x001FFFFFu;
        const uint32_t words = units * 8u;
        if (words == 0u) {
            return true;
        }

        const uint32_t src_base = source & ~3u;
        const uint32_t dst_base = destination & ~3u;
        if (fill) {
            const uint32_t value = bus_.Read32(src_base);
            for (uint32_t i = 0; i < words; ++i) {
                bus_.Write32(dst_base + (i * 4u), value);
            }
            return true;
        }

        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t value = bus_.Read32(src_base + (i * 4u));
            bus_.Write32(dst_base + (i * 4u), value);
        }
        return true;
    }

    const bool word_copy = (control & (1u << 26)) != 0u;
    const uint32_t count = control & 0x001FFFFFu;
    if (count == 0u) {
        return true;
    }

    if (word_copy) {
        const uint32_t src_base = source & ~3u;
        const uint32_t dst_base = destination & ~3u;
        if (fill) {
            const uint32_t value = bus_.Read32(src_base);
            for (uint32_t i = 0; i < count; ++i) {
                bus_.Write32(dst_base + (i * 4u), value);
            }
            return true;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t value = bus_.Read32(src_base + (i * 4u));
            bus_.Write32(dst_base + (i * 4u), value);
        }
        return true;
    }

    const uint32_t src_base = source & ~1u;
    const uint32_t dst_base = destination & ~1u;
    if (fill) {
        const uint16_t value = bus_.Read16(src_base);
        for (uint32_t i = 0; i < count; ++i) {
            bus_.Write16(dst_base + (i * 2u), value);
        }
        return true;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t value = bus_.Read16(src_base + (i * 2u));
        bus_.Write16(dst_base + (i * 2u), value);
    }
    return true;
}

bool Core::ExecuteIntrWait(bool force_vblank, std::string& error) {
    (void)error;
    if (force_vblank) {
        cpu_.r[0] = 1u;
        cpu_.r[1] = 1u;
    }

    uint16_t mask = static_cast<uint16_t>(cpu_.r[1] & 0x3FFFu);
    if (mask == 0u) {
        mask = 0x0001u;
    }
    const bool discard_old = (cpu_.r[0] & 0x1u) != 0u;

    uint16_t iflags = bus_.Read16(kInterruptFlagAddress);
    if (discard_old) {
        bus_.Write16(kInterruptFlagAddress, static_cast<uint16_t>(iflags & mask));
        iflags = bus_.Read16(kInterruptFlagAddress);
    }

    if ((iflags & mask) == 0u) {
        for (uint32_t i = 0u; i < (kCyclesPerFrame * 2u); ++i) {
            lcd_cycle_in_frame_ = (lcd_cycle_in_frame_ + 1u) % kCyclesPerFrame;
            AdvanceLcdTiming(lcd_cycle_in_frame_);
            iflags = bus_.Read16(kInterruptFlagAddress);
            if ((iflags & mask) != 0u) {
                break;
            }
        }
    }

    if ((iflags & mask) != 0u) {
        bus_.Write16(kInterruptFlagAddress, static_cast<uint16_t>(iflags & mask));
    }
    return true;
}

bool Core::ExecuteBgAffineSet() {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t count = cpu_.r[2];

    constexpr double kTwoPi = 6.28318530717958647692;

    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t src = source + (i * 20u);
        const int32_t tex_x = static_cast<int32_t>(bus_.Read32(src + 0u));
        const int32_t tex_y = static_cast<int32_t>(bus_.Read32(src + 4u));
        const int16_t disp_x = static_cast<int16_t>(bus_.Read16(src + 8u));
        const int16_t disp_y = static_cast<int16_t>(bus_.Read16(src + 10u));
        const int16_t scale_x_raw = static_cast<int16_t>(bus_.Read16(src + 12u));
        const int16_t scale_y_raw = static_cast<int16_t>(bus_.Read16(src + 14u));
        const uint16_t angle_raw = bus_.Read16(src + 16u);

        const double scale_x = static_cast<double>(scale_x_raw) / 256.0;
        const double scale_y = static_cast<double>(scale_y_raw) / 256.0;
        const double angle = (static_cast<double>(angle_raw) / 65536.0) * kTwoPi;
        const double cos_a = std::cos(angle);
        const double sin_a = std::sin(angle);

        const int16_t pa = static_cast<int16_t>(std::lround(cos_a * scale_x * 256.0));
        const int16_t pb = static_cast<int16_t>(std::lround(-sin_a * scale_x * 256.0));
        const int16_t pc = static_cast<int16_t>(std::lround(sin_a * scale_y * 256.0));
        const int16_t pd = static_cast<int16_t>(std::lround(cos_a * scale_y * 256.0));

        const int32_t start_x = tex_x -
            static_cast<int32_t>(pa) * static_cast<int32_t>(disp_x) -
            static_cast<int32_t>(pb) * static_cast<int32_t>(disp_y);
        const int32_t start_y = tex_y -
            static_cast<int32_t>(pc) * static_cast<int32_t>(disp_x) -
            static_cast<int32_t>(pd) * static_cast<int32_t>(disp_y);

        const uint32_t dst = destination + (i * 16u);
        bus_.Write16(dst + 0u, static_cast<uint16_t>(pa));
        bus_.Write16(dst + 2u, static_cast<uint16_t>(pb));
        bus_.Write16(dst + 4u, static_cast<uint16_t>(pc));
        bus_.Write16(dst + 6u, static_cast<uint16_t>(pd));
        bus_.Write32(dst + 8u, static_cast<uint32_t>(start_x));
        bus_.Write32(dst + 12u, static_cast<uint32_t>(start_y));
    }
    return true;
}

bool Core::ExecuteLz77UnComp(bool vram_mode, std::string& error) {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t header = bus_.Read32(source);
    if ((header & 0xFFu) != 0x10u) {
        error = "Invalid LZ77 header type in SWI";
        return false;
    }
    const uint32_t output_size = header >> 8u;
    if (output_size == 0u) {
        return true;
    }

    std::vector<uint8_t> output;
    output.reserve(output_size);
    uint32_t cursor = source + 4u;

    while (output.size() < output_size) {
        const uint8_t flags = bus_.Read8(cursor++);
        for (uint32_t bit = 0u; bit < 8u && output.size() < output_size; ++bit) {
            const bool compressed = (flags & (0x80u >> bit)) != 0u;
            if (!compressed) {
                output.push_back(bus_.Read8(cursor++));
                    return Reset(error);
                continue;
            }

            const uint8_t token_hi = bus_.Read8(cursor++);
            const uint8_t token_lo = bus_.Read8(cursor++);
            const uint32_t length = static_cast<uint32_t>((token_hi >> 4u) + 3u);
            const uint32_t displacement = ((static_cast<uint32_t>(token_hi & 0x0Fu) << 8u) | token_lo) + 1u;
            if (displacement > output.size()) {
                error = "Invalid LZ77 displacement in SWI";
                return false;
            }

            size_t copy_pos = output.size() - displacement;
            for (uint32_t i = 0u; i < length && output.size() < output_size; ++i) {
                const uint8_t value = output[copy_pos++];
                output.push_back(value);
            }
        }
    }

    if (!vram_mode) {
        for (uint32_t i = 0u; i < output_size; ++i) {
            bus_.Write8(destination + i, output[i]);
        }
        return true;
    }

    uint32_t out_addr = destination & ~1u;
    for (uint32_t i = 0u; i < output_size; i += 2u) {
        const uint16_t lo = output[i];
        const uint16_t hi = (i + 1u < output_size) ? output[i + 1u] : 0u;
        bus_.Write16(out_addr, static_cast<uint16_t>(lo | (hi << 8u)));
        out_addr += 2u;
    }
    return true;
}

bool Core::ExecuteRlUnComp(bool vram_mode, std::string& error) {
    const uint32_t source = cpu_.r[0];
    const uint32_t destination = cpu_.r[1];
    const uint32_t header = bus_.Read32(source);
    if ((header & 0xFFu) != 0x30u) {
        error = "Invalid RL header type in SWI";
        return false;
    }
    const uint32_t output_size = header >> 8u;
    if (output_size == 0u) {
        return true;
    }

    std::vector<uint8_t> output;
    output.reserve(output_size);
    uint32_t cursor = source + 4u;

    while (output.size() < output_size) {
        const uint8_t control = bus_.Read8(cursor++);
        const bool compressed = (control & 0x80u) != 0u;
        const uint32_t count = static_cast<uint32_t>(control & 0x7Fu) + (compressed ? 3u : 1u);
        if (compressed) {
            const uint8_t value = bus_.Read8(cursor++);
            for (uint32_t i = 0u; i < count && output.size() < output_size; ++i) {
                output.push_back(value);
            }
            continue;
        }

        for (uint32_t i = 0u; i < count && output.size() < output_size; ++i) {
            output.push_back(bus_.Read8(cursor++));
        }
    }

    if (!vram_mode) {
        for (uint32_t i = 0u; i < output_size; ++i) {
            bus_.Write8(destination + i, output[i]);
        }
        return true;
    }

    uint32_t out_addr = destination & ~1u;
    for (uint32_t i = 0u; i < output_size; i += 2u) {
        const uint16_t lo = output[i];
        const uint16_t hi = (i + 1u < output_size) ? output[i + 1u] : 0u;
        bus_.Write16(out_addr, static_cast<uint16_t>(lo | (hi << 8u)));
        out_addr += 2u;
    }
    return true;
}

bool Core::HandleUnsupportedArm(UnsupportedArmClass unsupported_class, uint32_t next_pc, std::string& error) {
    RecordUnsupportedArm(unsupported_class);
    last_unsupported_pc_ = cpu_.r[15];
    last_unsupported_instruction_ = cpu_.thumb ? bus_.Read16(cpu_.r[15]) : bus_.Read32(cpu_.r[15]);
    cpu_.r[15] = next_pc;
    if (unknown_arm_instruction_count_ > 10'000'000u) {
        halted_ = true;
        error = "In-house GBA core halted due to unsupported instruction volume (top class: " +
            std::string(DominantUnsupportedClassName()) +
            ", total=" + std::to_string(unknown_arm_instruction_count_) +
            ", last_pc=" + Hex32(last_unsupported_pc_) +
            ", last_instr=" + Hex32(last_unsupported_instruction_) + ")";
        return false;
    }
    return true;
}

void Core::RecordUnsupportedArm(UnsupportedArmClass unsupported_class) {
    ++unknown_arm_instruction_count_;
    switch (unsupported_class) {
    case UnsupportedArmClass::Generic:
        ++unsupported_arm_stats_.generic;
        break;
    case UnsupportedArmClass::Thumb:
        ++unsupported_arm_stats_.thumb;
        break;
    case UnsupportedArmClass::Multiply:
        ++unsupported_arm_stats_.multiply;
        break;
    case UnsupportedArmClass::BlockTransfer:
        ++unsupported_arm_stats_.block_transfer;
        break;
    case UnsupportedArmClass::SoftwareInterrupt:
        ++unsupported_arm_stats_.software_interrupt;
        break;
    }
}

const char* Core::DominantUnsupportedClassName() const noexcept {
    const uint64_t generic = unsupported_arm_stats_.generic;
    const uint64_t thumb = unsupported_arm_stats_.thumb;
    const uint64_t multiply = unsupported_arm_stats_.multiply;
    const uint64_t block_transfer = unsupported_arm_stats_.block_transfer;
    const uint64_t software_interrupt = unsupported_arm_stats_.software_interrupt;

    uint64_t best = generic;
    const char* name = "generic";
    if (thumb > best) {
        best = thumb;
        name = "thumb";
    }
    if (multiply > best) {
        best = multiply;
        name = "multiply";
    }
    if (block_transfer > best) {
        best = block_transfer;
        name = "block-transfer";
    }
    if (software_interrupt > best) {
        name = "software-interrupt";
    }
    return name;
}

bool Core::DecodeArmOperand2(
    uint32_t instruction, uint32_t fetch_pc, uint32_t& operand2, bool& shifter_carry) const {
    if ((instruction & (1u << 25)) != 0u) {
        const uint32_t imm8 = instruction & 0xFFu;
        const uint32_t rotate = ((instruction >> 8) & 0xFu) * 2u;
        operand2 = Ror(imm8, rotate);
        if (rotate != 0u) {
            shifter_carry = (operand2 & 0x80000000u) != 0;
        }
        return true;
    }

    operand2 = DecodeShiftedRegister(instruction, fetch_pc, shifter_carry);
    return true;
}

uint32_t Core::DecodeShiftedRegister(uint32_t instruction, uint32_t fetch_pc, bool& shifter_carry) const {
    const uint32_t rm = instruction & 0xFu;
    const uint32_t shift_type = (instruction >> 5) & 0x3u;
    const bool shift_by_register = (instruction & (1u << 4)) != 0u;

    uint32_t shift_amount = 0;
    if (shift_by_register) {
        const uint32_t rs = (instruction >> 8) & 0xFu;
        shift_amount = ReadArmRegister(rs, fetch_pc) & 0xFFu;
    } else {
        shift_amount = (instruction >> 7) & 0x1Fu;
    }

    const uint32_t value = ReadArmRegister(rm, fetch_pc);
    const bool carry_in = (cpu_.cpsr & (1u << 29)) != 0;

    switch (shift_type) {
    case 0x0: // LSL
        if (shift_amount == 0) {
            shifter_carry = carry_in;
            return value;
        }
        if (shift_amount < 32u) {
            shifter_carry = ((value >> (32u - shift_amount)) & 1u) != 0u;
            return value << shift_amount;
        }
        if (shift_amount == 32u) {
            shifter_carry = (value & 1u) != 0u;
            return 0u;
        }
        shifter_carry = false;
        return 0u;
    case 0x1: { // LSR
        uint32_t amount = shift_amount;
        if (!shift_by_register && amount == 0) {
            amount = 32;
        }
        if (amount == 0) {
            shifter_carry = carry_in;
            return value;
        }
        if (amount < 32u) {
            shifter_carry = ((value >> (amount - 1u)) & 1u) != 0u;
            return value >> amount;
        }
        if (amount == 32u) {
            shifter_carry = ((value >> 31u) & 1u) != 0u;
            return 0u;
        }
        shifter_carry = false;
        return 0u;
    }
    case 0x2: { // ASR
        uint32_t amount = shift_amount;
        if (!shift_by_register && amount == 0) {
            amount = 32;
        }
        if (amount == 0) {
            shifter_carry = carry_in;
            return value;
        }
        if (amount < 32u) {
            shifter_carry = ((value >> (amount - 1u)) & 1u) != 0u;
            return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
        }
        const bool sign = (value & 0x80000000u) != 0u;
        shifter_carry = sign;
        return sign ? 0xFFFFFFFFu : 0u;
    }
    case 0x3: { // ROR / RRX
        if (shift_amount == 0) {
            if (!shift_by_register) {
                shifter_carry = (value & 1u) != 0u;
                return (carry_in ? 0x80000000u : 0u) | (value >> 1);
            }
            shifter_carry = carry_in;
            return value;
        }

        const uint32_t amount = shift_amount & 31u;
        if (amount == 0u) {
            shifter_carry = ((value >> 31u) & 1u) != 0u;
            return value;
        }
        const uint32_t rotated = Ror(value, amount);
        shifter_carry = ((rotated >> 31u) & 1u) != 0u;
        return rotated;
    }
    default:
        shifter_carry = carry_in;
        return value;
    }
}

uint32_t Core::ReadArmRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    if (reg_index == 15u) {
        return fetch_pc;
    }
    return cpu_.r[reg_index & 0xFu];
}

void Core::WriteArmRegister(uint32_t reg_index, uint32_t value) {
    if (reg_index == 15u) {
        cpu_.r[15] = cpu_.thumb ? (value & ~1u) : (value & ~3u);
        return;
    }
    cpu_.r[reg_index & 0xFu] = value;
}

uint32_t Core::ReadThumbRegister(uint32_t reg_index, uint32_t fetch_pc) const {
    if (reg_index == 15u) {
        return fetch_pc;
    }
    return cpu_.r[reg_index & 0xFu];
}

void Core::WriteThumbRegister(uint32_t reg_index, uint32_t value) {
    if (reg_index == 15u) {
        cpu_.r[15] = value & ~1u;
        return;
    }
    cpu_.r[reg_index & 0xFu] = value;
}

void Core::SetNFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 31)) | (value ? (1u << 31) : 0u);
}

void Core::SetZFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 30)) | (value ? (1u << 30) : 0u);
}

void Core::SetCFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 29)) | (value ? (1u << 29) : 0u);
}

void Core::SetVFlag(bool value) {
    cpu_.cpsr = (cpu_.cpsr & ~(1u << 28)) | (value ? (1u << 28) : 0u);
}

void Core::SetNZFlags(uint32_t value) {
    SetNFlag((value & 0x80000000u) != 0u);
    SetZFlag(value == 0u);
}

bool Core::ConditionPassed(uint32_t instruction) const {
    const uint32_t cond = (instruction >> 28) & 0xFu;
    const bool n = (cpu_.cpsr & (1u << 31)) != 0;
    const bool z = (cpu_.cpsr & (1u << 30)) != 0;
    const bool c = (cpu_.cpsr & (1u << 29)) != 0;
    const bool v = (cpu_.cpsr & (1u << 28)) != 0;

    switch (cond) {
    case 0x0:
        return z;
    case 0x1:
        return !z;
    case 0x2:
        return c;
    case 0x3:
        return !c;
    case 0x4:
        return n;
    case 0x5:
        return !n;
    case 0x6:
        return v;
    case 0x7:
        return !v;
    case 0x8:
        return c && !z;
    case 0x9:
        return !c || z;
    case 0xA:
        return n == v;
    case 0xB:
        return n != v;
    case 0xC:
        return !z && (n == v);
    case 0xD:
        return z || (n != v);
    case 0xE:
        return true;
    default:
        return false;
    }
}

uint32_t Core::SignExtend24(uint32_t value) {
    if ((value & 0x00800000u) != 0) {
        return value | 0xFF000000u;
    }
    return value;
}

int32_t Core::SignExtend(uint32_t value, uint32_t bits) {
    const uint32_t shift = 32u - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}

bool Core::IsValidCpuMode(uint32_t mode) noexcept {
    switch (mode) {
    case kCpuModeUser:
    case kCpuModeFiq:
    case kCpuModeIrq:
    case kCpuModeSupervisor:
    case kCpuModeAbort:
    case kCpuModeUndefined:
    case kCpuModeSystem:
        return true;
    default:
        return false;
    }
}

bool Core::ModeHasSpsr(uint32_t mode) noexcept {
    return mode == kCpuModeFiq ||
        mode == kCpuModeIrq ||
        mode == kCpuModeSupervisor ||
        mode == kCpuModeAbort ||
        mode == kCpuModeUndefined;
}

uint32_t Core::CurrentMode() const noexcept {
    return cpu_.cpsr & kCpuModeMask;
}

bool Core::CurrentModeIsPrivileged() const noexcept {
    return CurrentMode() != kCpuModeUser;
}

void Core::SaveBankedRegistersForMode(uint32_t mode) noexcept {
    switch (mode) {
    case kCpuModeFiq:
        for (uint32_t reg = 8u; reg <= 14u; ++reg) {
            cpu_.banked_r8_r14_fiq[reg - 8u] = cpu_.r[reg];
        }
        break;
    case kCpuModeIrq:
        cpu_.banked_r13_r14_irq[0] = cpu_.r[13];
        cpu_.banked_r13_r14_irq[1] = cpu_.r[14];
        break;
    case kCpuModeSupervisor:
        cpu_.banked_r13_r14_svc[0] = cpu_.r[13];
        cpu_.banked_r13_r14_svc[1] = cpu_.r[14];
        break;
    case kCpuModeAbort:
        cpu_.banked_r13_r14_abt[0] = cpu_.r[13];
        cpu_.banked_r13_r14_abt[1] = cpu_.r[14];
        break;
    case kCpuModeUndefined:
        cpu_.banked_r13_r14_und[0] = cpu_.r[13];
        cpu_.banked_r13_r14_und[1] = cpu_.r[14];
        break;
    default:
        break;
    }
}

void Core::RestoreBankedRegistersForMode(uint32_t mode) noexcept {
    switch (mode) {
    case kCpuModeFiq:
        for (uint32_t reg = 8u; reg <= 14u; ++reg) {
            cpu_.r[reg] = cpu_.banked_r8_r14_fiq[reg - 8u];
        }
        break;
    case kCpuModeIrq:
        cpu_.r[13] = cpu_.banked_r13_r14_irq[0];
        cpu_.r[14] = cpu_.banked_r13_r14_irq[1];
        break;
    case kCpuModeSupervisor:
        cpu_.r[13] = cpu_.banked_r13_r14_svc[0];
        cpu_.r[14] = cpu_.banked_r13_r14_svc[1];
        break;
    case kCpuModeAbort:
        cpu_.r[13] = cpu_.banked_r13_r14_abt[0];
        cpu_.r[14] = cpu_.banked_r13_r14_abt[1];
        break;
    case kCpuModeUndefined:
        cpu_.r[13] = cpu_.banked_r13_r14_und[0];
        cpu_.r[14] = cpu_.banked_r13_r14_und[1];
        break;
    default:
        break;
    }
}

void Core::SwitchCpuMode(uint32_t new_mode) noexcept {
    const uint32_t current_mode = CurrentMode();
    if (new_mode == current_mode || !IsValidCpuMode(new_mode)) {
        return;
    }

    SaveBankedRegistersForMode(current_mode);
    cpu_.cpsr = (cpu_.cpsr & ~kCpuModeMask) | new_mode;
    RestoreBankedRegistersForMode(new_mode);
}

uint32_t Core::ReadSpsrForMode(uint32_t mode) const noexcept {
    switch (mode) {
    case kCpuModeFiq:
        return cpu_.spsr_fiq;
    case kCpuModeIrq:
        return cpu_.spsr_irq;
    case kCpuModeSupervisor:
        return cpu_.spsr_svc;
    case kCpuModeAbort:
        return cpu_.spsr_abt;
    case kCpuModeUndefined:
        return cpu_.spsr_und;
    default:
        return 0u;
    }
}

void Core::WriteSpsrForMode(uint32_t mode, uint32_t value) noexcept {
    switch (mode) {
    case kCpuModeFiq:
        cpu_.spsr_fiq = value;
        break;
    case kCpuModeIrq:
        cpu_.spsr_irq = value;
        break;
    case kCpuModeSupervisor:
        cpu_.spsr_svc = value;
        break;
    case kCpuModeAbort:
        cpu_.spsr_abt = value;
        break;
    case kCpuModeUndefined:
        cpu_.spsr_und = value;
        break;
    default:
        break;
    }
}

uint32_t Core::ReadCurrentSpsr() const noexcept {
    return ReadSpsrForMode(CurrentMode());
}

bool Core::WriteCurrentSpsr(uint32_t value) noexcept {
    const uint32_t mode = CurrentMode();
    if (!ModeHasSpsr(mode)) {
        return false;
    }
    WriteSpsrForMode(mode, value);
    return true;
}

void Core::WriteCpsr(uint32_t value) noexcept {
    const uint32_t current_mode = CurrentMode();
    uint32_t next_mode = value & kCpuModeMask;
    if (!IsValidCpuMode(next_mode)) {
        next_mode = current_mode;
        value = (value & ~kCpuModeMask) | next_mode;
    }

    if (next_mode != current_mode) {
        SaveBankedRegistersForMode(current_mode);
        RestoreBankedRegistersForMode(next_mode);
    }

    cpu_.cpsr = value;
    cpu_.thumb = (cpu_.cpsr & kCpsrThumbBit) != 0u;
}

void Core::EnterException(
    uint32_t target_mode,
    uint32_t handler_address,
    bool handler_thumb,
    uint32_t return_address,
    bool mask_irq) noexcept {
    const uint32_t saved_cpsr = cpu_.cpsr;
    WriteSpsrForMode(target_mode, saved_cpsr);

    uint32_t exception_cpsr = (saved_cpsr & ~kCpuModeMask) | target_mode;
    if (mask_irq) {
        exception_cpsr |= kCpsrIrqDisableBit;
    }
    if (handler_thumb) {
        exception_cpsr |= kCpsrThumbBit;
    } else {
        exception_cpsr &= ~kCpsrThumbBit;
    }

    WriteCpsr(exception_cpsr);
    cpu_.r[14] = return_address;
    cpu_.r[15] = handler_thumb ? (handler_address & ~1u) : (handler_address & ~3u);
}

void Core::ServicePendingInterrupts() noexcept {
    if ((cpu_.cpsr & kCpsrIrqDisableBit) != 0u) {
        return;
    }

    const uint16_t ime = bus_.Read16(kInterruptMasterEnableAddress);
    if ((ime & 0x0001u) == 0u) {
        return;
    }
    const uint16_t ie = bus_.Read16(kInterruptEnableAddress);
    const uint16_t iflags = bus_.Read16(kInterruptFlagAddress);
    const uint16_t pending = static_cast<uint16_t>(ie & iflags);
    if (pending == 0u) {
        return;
    }

    bus_.Write16(kInterruptFlagAddress, pending);
    const uint32_t callback = bus_.Read32(kIrqCallbackAddress);
    if (callback == 0u || callback == 0xFFFFFFFFu) {
        return;
    }

    const bool callback_thumb = (callback & 1u) != 0u;
    const uint32_t return_address = cpu_.r[15] | (cpu_.thumb ? 1u : 0u);
    halted_ = false;
    EnterException(kCpuModeIrq, callback, callback_thumb, return_address, true);
}

void Core::AdvanceLcdTiming(uint32_t elapsed_cycles) noexcept {
    const uint16_t vcount = static_cast<uint16_t>((elapsed_cycles / kCyclesPerScanline) % kTotalScanlines);
    const uint32_t line_cycle = elapsed_cycles % kCyclesPerScanline;
    const bool vblank = vcount >= kVisibleScanlines;
    const bool hblank = line_cycle >= 1006u;
    if (vblank && !in_vblank_) {
        bus_.SetInterruptRequest(0x0001u);
        bus_.TriggerDmaTiming(0x1u);
    }
    if (hblank && !in_hblank_) {
        bus_.TriggerDmaTiming(0x2u);
    }
    in_vblank_ = vblank;
    in_hblank_ = hblank;
    bus_.SetLcdStatus(vcount, vblank, hblank);
}

void Core::UpdateVideoFrame() {
    const uint16_t dispcnt = bus_.Read16(kDispCntAddress);
    if ((dispcnt & (1u << 7u)) != 0u) {
        video_buffer_.fill(0xFFFFFFFFu);
        return;
    }

    const auto read_palette_color = [&](uint16_t palette_index) -> uint32_t {
        const uint16_t raw = bus_.Read16(kPaletteBase + static_cast<uint32_t>(palette_index) * 2u);
        return Bgr555ToAbgr8888(raw);
    };

    const uint32_t backdrop = read_palette_color(0u);
    const uint32_t mode = dispcnt & kModeMask;
    const bool bg2_enabled = (dispcnt & (1u << 10u)) != 0u;

    if (mode == 3u && bg2_enabled) {
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(kVideoWidth) + static_cast<size_t>(x);
                const uint32_t addr = kVramBase + ((y * kVideoWidth + x) * 2u);
                video_buffer_[idx] = Bgr555ToAbgr8888(bus_.Read16(addr));
            }
        }
        return;
    }

    if (mode == 4u && bg2_enabled) {
        const uint32_t page_offset = (dispcnt & (1u << 4u)) != 0u ? 0x0000A000u : 0u;
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(kVideoWidth) + static_cast<size_t>(x);
                const uint8_t palette_index = bus_.Read8(kVramBase + page_offset + (y * kVideoWidth + x));
                video_buffer_[idx] = read_palette_color(palette_index);
            }
        }
        return;
    }

    if (mode == 5u && bg2_enabled) {
        const uint32_t page_offset = (dispcnt & (1u << 4u)) != 0u ? 0x0000A000u : 0u;
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(kVideoWidth) + static_cast<size_t>(x);
                if (x < 160u && y < 128u) {
                    const uint32_t addr = kVramBase + page_offset + ((y * 160u + x) * 2u);
                    video_buffer_[idx] = Bgr555ToAbgr8888(bus_.Read16(addr));
                } else {
                    video_buffer_[idx] = backdrop;
                }
            }
        }
        return;
    }

    if (mode != 0u) {
        video_buffer_.fill(backdrop);
        return;
    }

    std::array<bool, 4u> bg_enabled{};
    std::array<uint16_t, 4u> bgcnt{};
    std::array<uint16_t, 4u> hofs{};
    std::array<uint16_t, 4u> vofs{};
    for (uint32_t bg = 0; bg < 4u; ++bg) {
        bg_enabled[bg] = (dispcnt & (1u << (8u + bg))) != 0u;
        bgcnt[bg] = bus_.Read16(kBgCntAddressBase + (bg * 2u));
        hofs[bg] = static_cast<uint16_t>(bus_.Read16(kBgHofsAddressBase + (bg * 4u)) & 0x01FFu);
        vofs[bg] = static_cast<uint16_t>(bus_.Read16(kBgVofsAddressBase + (bg * 4u)) & 0x01FFu);
    }

    auto sample_mode0_text_bg = [&](uint32_t bg, uint32_t screen_x, uint32_t screen_y, uint32_t& out_color) -> bool {
        if (!bg_enabled[bg]) {
            return false;
        }

        const uint16_t cnt = bgcnt[bg];
        const bool color_256 = (cnt & (1u << 7u)) != 0u;
        const uint32_t char_base = static_cast<uint32_t>((cnt >> 2u) & 0x3u) * 0x4000u;
        const uint32_t screen_base = static_cast<uint32_t>((cnt >> 8u) & 0x1Fu) * 0x0800u;
        const uint32_t size = (cnt >> 14u) & 0x3u;
        const uint32_t width = (size == 0u || size == 2u) ? 256u : 512u;
        const uint32_t height = (size == 0u || size == 1u) ? 256u : 512u;

        const uint32_t x = (screen_x + hofs[bg]) & (width - 1u);
        const uint32_t y = (screen_y + vofs[bg]) & (height - 1u);
        const uint32_t tile_x = x >> 3u;
        const uint32_t tile_y = y >> 3u;
        const uint32_t pixel_x = x & 0x7u;
        const uint32_t pixel_y = y & 0x7u;
        const uint32_t block_x = tile_x >> 5u;
        const uint32_t block_y = tile_y >> 5u;
        const uint32_t local_tile_x = tile_x & 31u;
        const uint32_t local_tile_y = tile_y & 31u;

        uint32_t map_block = 0u;
        switch (size) {
        case 0u:
            map_block = 0u;
            break;
        case 1u:
            map_block = block_x;
            break;
        case 2u:
            map_block = block_y;
            break;
        case 3u:
            map_block = (block_y * 2u) + block_x;
            break;
        default:
            map_block = 0u;
            break;
        }

        const uint32_t map_addr = kVramBase + screen_base + (map_block * 0x0800u) +
            ((local_tile_y * 32u + local_tile_x) * 2u);
        const uint16_t map_entry = bus_.Read16(map_addr);
        const uint32_t tile_index = map_entry & 0x03FFu;
        const bool hflip = (map_entry & (1u << 10u)) != 0u;
        const bool vflip = (map_entry & (1u << 11u)) != 0u;
        const uint32_t palette_bank = (map_entry >> 12u) & 0xFu;
        const uint32_t tile_px = hflip ? (7u - pixel_x) : pixel_x;
        const uint32_t tile_py = vflip ? (7u - pixel_y) : pixel_y;

        uint16_t palette_index = 0u;
        bool transparent = false;
        if (color_256) {
            const uint32_t tile_addr = kVramBase + char_base + (tile_index * 64u) + (tile_py * 8u) + tile_px;
            palette_index = bus_.Read8(tile_addr);
            transparent = palette_index == 0u;
        } else {
            const uint32_t tile_addr = kVramBase + char_base + (tile_index * 32u) + (tile_py * 4u) + (tile_px >> 1u);
            const uint8_t packed = bus_.Read8(tile_addr);
            const uint16_t texel = (tile_px & 1u) == 0u ? (packed & 0x0Fu) : ((packed >> 4u) & 0x0Fu);
            palette_index = static_cast<uint16_t>((palette_bank << 4u) | texel);
            transparent = texel == 0u;
        }

        if (transparent) {
            return false;
        }

        out_color = read_palette_color(palette_index);
        return true;
    };

    for (uint32_t y = 0; y < kVideoHeight; ++y) {
        for (uint32_t x = 0; x < kVideoWidth; ++x) {
            uint32_t color = backdrop;
            for (uint32_t priority = 4u; priority > 0u; --priority) {
                const uint32_t priority_level = priority - 1u;
                for (uint32_t bg = 4u; bg > 0u; --bg) {
                    const uint32_t bg_index = bg - 1u;
                    if (!bg_enabled[bg_index]) {
                        continue;
                    }
                    if ((bgcnt[bg_index] & 0x3u) != priority_level) {
                        continue;
                    }

                    uint32_t sampled = 0u;
                    if (sample_mode0_text_bg(bg_index, x, y, sampled)) {
                        color = sampled;
                    }
                }
            }
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(kVideoWidth) + static_cast<size_t>(x);
            video_buffer_[idx] = color;
        }
    }
}

uint32_t Core::Ror(uint32_t value, uint32_t amount) {
    const uint32_t rotated = amount & 31u;
    if (rotated == 0u) {
        return value;
    }
    return (value >> rotated) | (value << (32u - rotated));
}

Core::AddResult Core::AddWithCarry(uint32_t lhs, uint32_t rhs, bool carry_in) {
    const uint64_t carry = carry_in ? 1ull : 0ull;
    const uint64_t wide = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs) + carry;
    AddResult out;
    out.value = static_cast<uint32_t>(wide);
    out.carry = (wide >> 32u) != 0u;

    const bool lhs_sign = (lhs & 0x80000000u) != 0u;
    const bool rhs_sign = (rhs & 0x80000000u) != 0u;
    const bool out_sign = (out.value & 0x80000000u) != 0u;
    out.overflow = (lhs_sign == rhs_sign) && (lhs_sign != out_sign);
    return out;
}

} // namespace emugbds::ingba
