#include "emugbds/inds/MemoryBus.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace emugbds::inds {
namespace {

constexpr uint32_t kIoRegion = 0x04u;

// Byte-lane mask of an aligned access of `size` bytes within its 32-bit word.
uint32_t LaneMask(uint32_t address, uint32_t size) {
    const uint32_t lanes = size == 4u ? 0xFFFFFFFFu : ((1u << (size * 8u)) - 1u);
    return lanes << ((address & 3u) * 8u);
}

bool LoadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open file: " + path.string();
        return false;
    }
    out_bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (out_bytes.empty()) {
        error = "File is empty: " + path.string();
        return false;
    }
    // Word-multiple sizes keep aligned 32-bit reads inside mirrored images.
    out_bytes.resize((out_bytes.size() + 3u) & ~size_t{3u}, 0u);
    return true;
}

} // namespace

MemoryBus::MemoryBus()
    : main_ram_(kMainRamSize, 0u),
      shared_wram_(kSharedWramSize, 0u),
      arm7_wram_(kArm7WramSize, 0u),
      itcm_(Cp15::kItcmPhysicalSize, 0u),
      dtcm_(Cp15::kDtcmPhysicalSize, 0u) {
    cp15_.Reset();
    io_.card().AttachRom(&nds_rom_);
    io_.Reset();
}

bool MemoryBus::LoadBios7(const std::filesystem::path& bios_path, std::string& error) {
    return LoadWholeFile(bios_path, bios7_, error);
}

bool MemoryBus::LoadBios9(const std::filesystem::path& bios_path, std::string& error) {
    return LoadWholeFile(bios_path, bios9_, error);
}

bool MemoryBus::LoadFirmware(const std::filesystem::path& firmware_path, std::string& error) {
    return LoadWholeFile(firmware_path, firmware_, error);
}

bool MemoryBus::LoadNdsRom(const std::filesystem::path& rom_path, std::string& error) {
    return LoadWholeFile(rom_path, nds_rom_, error);
}

bool MemoryBus::LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error) {
    return LoadWholeFile(rom_path, slot2_rom_, error);
}

void MemoryBus::ClearNdsRom() {
    nds_rom_.clear();
}

void MemoryBus::ClearSlot2Rom() {
    slot2_rom_.clear();
}

void MemoryBus::ResetMemory() {
    for (auto* region : {&main_ram_, &shared_wram_, &arm7_wram_, &itcm_, &dtcm_}) {
        std::fill(region->begin(), region->end(), 0u);
    }
    wramcnt_ = 3u; // Power-on: all shared WRAM belongs to the ARM7.
    cp15_.Reset();
    video_.Reset();
    for (Engine2d& engine : engines_) {
        engine.Reset();
    }
    io_.Reset();
    arm9_unmapped_ = {};
    arm7_unmapped_ = {};
}


uint8_t* MemoryBus::Arm9Pointer(uint32_t address, bool write, bool use_tcm) {
    // ITCM has priority over DTCM; neither is mirrored outside its virtual size.
    // A TCM in load mode only takes writes.
    if (use_tcm && cp15_.itcm_enabled() && (write || !cp15_.itcm_load_mode()) && address < cp15_.itcm_size()) {
        return &itcm_[address & (Cp15::kItcmPhysicalSize - 1u)];
    }
    if (use_tcm && cp15_.dtcm_enabled() && (write || !cp15_.dtcm_load_mode()) &&
        static_cast<uint32_t>(address - cp15_.dtcm_base()) < cp15_.dtcm_size()) {
        return &dtcm_[(address - cp15_.dtcm_base()) & (Cp15::kDtcmPhysicalSize - 1u)];
    }
    switch (address >> 24u) {
    case 0x02u:
        return &main_ram_[address & (kMainRamSize - 1u)];
    case 0x03u:
        switch (wramcnt_) {
        case 0u: return &shared_wram_[address & 0x7FFFu];
        case 1u: return &shared_wram_[0x4000u + (address & 0x3FFFu)];
        case 2u: return &shared_wram_[address & 0x3FFFu];
        default: return nullptr; // No WRAM allocated to the ARM9.
        }
    case 0x05u:
    case 0x06u:
    case 0x07u:
        return video_.CpuPointer(true, address);
    case 0x08u:
    case 0x09u:
        return !write && !slot2_rom_.empty() ? &slot2_rom_[(address - kSlot2Base) % slot2_rom_.size()] : nullptr;
    case 0xFFu: // The 32 KiB BIOS region is not mirrored beyond itself.
        return !write && address - kArm9BiosBase < kArm9BiosRegionSize && !bios9_.empty()
            ? &bios9_[(address - kArm9BiosBase) % bios9_.size()]
            : nullptr;
    default:
        return nullptr;
    }
}


uint8_t* MemoryBus::Arm7Pointer(uint32_t address, bool write) {
    switch (address >> 24u) {
    case 0x00u:
        return !write && address < bios7_.size() ? &bios7_[address] : nullptr;
    case 0x02u:
        return &main_ram_[address & (kMainRamSize - 1u)];
    case 0x03u:
        if (address >= 0x03800000u) {
            return &arm7_wram_[address & (kArm7WramSize - 1u)];
        }
        switch (wramcnt_) {
        case 0u: return &arm7_wram_[address & (kArm7WramSize - 1u)]; // 0K shared: ARM7 WRAM mirrors.
        case 1u: return &shared_wram_[address & 0x3FFFu];
        case 2u: return &shared_wram_[0x4000u + (address & 0x3FFFu)];
        default: return &shared_wram_[address & 0x7FFFu];
        }
    case 0x06u:
        return video_.CpuPointer(false, address); // VRAM C/D in ARM7 mode
    case 0x08u:
    case 0x09u:
        return !write && !slot2_rom_.empty() ? &slot2_rom_[(address - kSlot2Base) % slot2_rom_.size()] : nullptr;
    default:
        return nullptr;
    }
}


uint32_t MemoryBus::Read(bool arm9, uint32_t address, uint32_t size, bool use_tcm) const {
    if ((address & (size - 1u)) != 0u) {
        uint32_t value = 0u;
        for (uint32_t i = 0u; i < size; ++i) {
            value |= Read(arm9, address + i, 1u, use_tcm) << (8u * i);
        }
        return value;
    }
    // Memory reads never modify state; I/O reads can (FIFO pops), as on hardware.
    auto& self = const_cast<MemoryBus&>(*this);
    if (const uint8_t* bytes = arm9 ? self.Arm9Pointer(address, false, use_tcm) : self.Arm7Pointer(address, false)) {
        uint32_t value = 0u;
        for (uint32_t i = 0u; i < size; ++i) {
            value |= static_cast<uint32_t>(bytes[i]) << (8u * i);
        }
        return value;
    }

    const bool io = (address >> 24u) == kIoRegion;
    const uint32_t mask = LaneMask(address, size);
    uint32_t word = 0u;
    if (io && self.io_.ReadWord(arm9 ? 0 : 1, address & ~3u, mask, word)) {
        return (word & mask) >> ((address & 3u) * 8u);
    }
    auto& log = arm9 ? arm9_unmapped_ : arm7_unmapped_;
    AccessLog::Count(log.reads, io ? address : (address & 0xFFFF0000u));
    return 0u; // Undefined I/O reads as zero on the DS.
}


void MemoryBus::Write(bool arm9, uint32_t address, uint32_t value, uint32_t size, bool use_tcm) {
    if ((address & (size - 1u)) != 0u) {
        for (uint32_t i = 0u; i < size; ++i) {
            Write(arm9, address + i, (value >> (8u * i)) & 0xFFu, 1u, use_tcm);
        }
        return;
    }
    if (arm9 && size == 1u && (address >> 24u) >= 0x05u && (address >> 24u) <= 0x07u) {
        return; // The ARM9 cannot write single bytes to palette, VRAM, or OAM.
    }
    if (uint8_t* bytes = arm9 ? Arm9Pointer(address, true, use_tcm) : Arm7Pointer(address, true)) {
        for (uint32_t i = 0u; i < size; ++i) {
            bytes[i] = static_cast<uint8_t>(value >> (8u * i));
        }
        return;
    }

    const bool io = (address >> 24u) == kIoRegion;
    const uint32_t shift = (address & 3u) * 8u;
    if (io && io_.WriteWord(arm9 ? 0 : 1, address & ~3u, size == 4u ? value : (value << shift), LaneMask(address, size))) {
        return;
    }
    auto& log = arm9 ? arm9_unmapped_ : arm7_unmapped_;
    AccessLog::Count(log.writes, io ? address : (address & 0xFFFF0000u));
}

} // namespace emugbds::inds
