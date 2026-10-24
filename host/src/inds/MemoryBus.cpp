#include "emugbds/inds/MemoryBus.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace emugbds::inds {
namespace {

constexpr uint32_t kIoRegion = 0x04u;
constexpr uint32_t kWramCntAddress = 0x04000247u;  // ARM9 WRAMCNT (R/W)
constexpr uint32_t kWramStatAddress = 0x04000241u; // ARM7 WRAMSTAT (R)

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
    arm9_unmapped_ = {};
    arm7_unmapped_ = {};
}


uint8_t* MemoryBus::Arm9Pointer(uint32_t address, bool write) {
    // ITCM has priority over DTCM; neither is mirrored outside its virtual size.
    // A TCM in load mode only takes writes.
    if (cp15_.itcm_enabled() && (write || !cp15_.itcm_load_mode()) && address < cp15_.itcm_size()) {
        return &itcm_[address & (Cp15::kItcmPhysicalSize - 1u)];
    }
    if (cp15_.dtcm_enabled() && (write || !cp15_.dtcm_load_mode()) &&
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
    case 0x08u:
    case 0x09u:
        return !write && !slot2_rom_.empty() ? &slot2_rom_[(address - kSlot2Base) % slot2_rom_.size()] : nullptr;
    default:
        return nullptr;
    }
}


bool MemoryBus::ReadIoByte(bool arm9, uint32_t address, uint8_t& value) const {
    value = 0u;
    if ((arm9 && address == kWramCntAddress) || (!arm9 && address == kWramStatAddress)) {
        value = wramcnt_;
        return true;
    }
    return false;
}


bool MemoryBus::WriteIoByte(bool arm9, uint32_t address, uint8_t value) {
    if (arm9 && address == kWramCntAddress) {
        wramcnt_ = static_cast<uint8_t>(value & 0x3u);
        return true;
    }
    return false;
}


uint32_t MemoryBus::Read(bool arm9, uint32_t address, uint32_t size) const {
    if ((address & (size - 1u)) != 0u) {
        uint32_t value = 0u;
        for (uint32_t i = 0u; i < size; ++i) {
            value |= Read(arm9, address + i, 1u) << (8u * i);
        }
        return value;
    }
    // Reads never modify memory; the pointer lookups are shared with writes.
    auto& self = const_cast<MemoryBus&>(*this);
    if (const uint8_t* bytes = arm9 ? self.Arm9Pointer(address, false) : self.Arm7Pointer(address, false)) {
        uint32_t value = 0u;
        for (uint32_t i = 0u; i < size; ++i) {
            value |= static_cast<uint32_t>(bytes[i]) << (8u * i);
        }
        return value;
    }

    // Each byte lane is decoded on its own; undefined I/O lanes read as zero on the DS.
    const bool io = (address >> 24u) == kIoRegion;
    uint32_t value = 0u;
    bool known = io;
    for (uint32_t i = 0u; io && i < size; ++i) {
        uint8_t byte = 0u;
        if (!ReadIoByte(arm9, address + i, byte)) {
            known = false;
        }
        value |= static_cast<uint32_t>(byte) << (8u * i);
    }
    if (!known) {
        auto& log = arm9 ? arm9_unmapped_ : arm7_unmapped_;
        ++log.reads[io ? address : (address & 0xFFFF0000u)];
    }
    return value;
}


void MemoryBus::Write(bool arm9, uint32_t address, uint32_t value, uint32_t size) {
    if ((address & (size - 1u)) != 0u) {
        for (uint32_t i = 0u; i < size; ++i) {
            Write(arm9, address + i, (value >> (8u * i)) & 0xFFu, 1u);
        }
        return;
    }
    if (uint8_t* bytes = arm9 ? Arm9Pointer(address, true) : Arm7Pointer(address, true)) {
        for (uint32_t i = 0u; i < size; ++i) {
            bytes[i] = static_cast<uint8_t>(value >> (8u * i));
        }
        return;
    }

    bool known = (address >> 24u) == kIoRegion;
    for (uint32_t i = 0u; i < size; ++i) {
        if ((address >> 24u) == kIoRegion && !WriteIoByte(arm9, address + i, static_cast<uint8_t>(value >> (8u * i)))) {
            known = false;
        }
    }
    if (!known) {
        auto& log = arm9 ? arm9_unmapped_ : arm7_unmapped_;
        ++log.writes[(address >> 24u) == kIoRegion ? address : (address & 0xFFFF0000u)];
    }
}

} // namespace emugbds::inds
