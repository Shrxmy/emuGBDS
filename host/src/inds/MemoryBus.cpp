#include "emugbds/inds/MemoryBus.h"

#include "emugbds/core/SaveFile.h"

#include <algorithm>
#include <cstring>
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
    // Word-multiple sizes keep aligned 32-bit reads inside mirrored images.
    return core::ReadWholeFile(path, out_bytes, error, 4u);
}

} // namespace

MemoryBus::MemoryBus()
    : main_ram_(kMainRamSize, 0u),
      shared_wram_(kSharedWramSize, 0u),
      arm7_wram_(kArm7WramSize, 0u),
      itcm_(Cp15::kItcmPhysicalSize, 0u),
      dtcm_(Cp15::kDtcmPhysicalSize, 0u) {
    cp15_.Reset();
    RefreshAccessTimes();
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
    if (!LoadWholeFile(firmware_path, firmware_, error)) {
        return false;
    }
    firmware_save_path_ = firmware_path;
    firmware_save_path_.replace_extension(".sav");
    std::error_code exists_error;
    if (std::filesystem::exists(firmware_save_path_, exists_error)) {
        std::vector<uint8_t> saved;
        if (!LoadWholeFile(firmware_save_path_, saved, error)) {
            return false;
        }
        if (saved.size() != firmware_.size()) {
            error = "Firmware save " + firmware_save_path_.string() + " does not match the size of " + firmware_path.string();
            return false; // refusing beats silently dropping the user's settings
        }
        firmware_ = std::move(saved);
    }
    return true;
}

bool MemoryBus::LoadNdsRom(const std::filesystem::path& rom_path, std::string& error) {
    return LoadWholeFile(rom_path, nds_rom_, error);
}

bool MemoryBus::LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error) {
    std::vector<uint8_t> rom;
    if (!LoadWholeFile(rom_path, rom, error)) {
        return false;
    }
    // The cartridge brings its save chip (the same <rom>.sav GBA mode uses). EEPROMs are not
    // mapped in the DS slot, so only SRAM and FLASH games show their saves here.
    std::filesystem::path save_path = rom_path;
    save_path.replace_extension(".sav");
    ingba::Backup backup;
    if (!backup.Load(ingba::Backup::Detect(rom), save_path, error)) {
        return false;
    }
    slot2_rom_ = std::move(rom);
    slot2_backup_ = std::move(backup);
    return true;
}

void MemoryBus::ClearNdsRom() {
    nds_rom_.clear();
}

void MemoryBus::ClearSlot2Rom() {
    slot2_rom_.clear();
    slot2_backup_ = ingba::Backup{};
}

void MemoryBus::ResetMemory() {
    for (auto* region : {&main_ram_, &shared_wram_, &arm7_wram_, &itcm_, &dtcm_}) {
        std::fill(region->begin(), region->end(), 0u);
    }
    wramcnt_ = 3u; // Power-on: all shared WRAM belongs to the ARM7.
    cp15_.Reset();
    RefreshAccessTimes();
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
        return !write && !slot2_rom_.empty() && io_.Slot2Owner() == 0 ? &slot2_rom_[(address - kSlot2Base) % slot2_rom_.size()] : nullptr;
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
        return !write && !slot2_rom_.empty() && io_.Slot2Owner() == 1 ? &slot2_rom_[(address - kSlot2Base) % slot2_rom_.size()] : nullptr;
    default:
        return nullptr;
    }
}


uint32_t MemoryBus::AccessCycles(bool arm9, uint32_t address, uint32_t width, bool sequential, bool code) const {
    // NDS9: TCM runs at the full 66 MHz.
    if (arm9 && cp15_.itcm_enabled() && address < cp15_.itcm_size()) {
        return 1u;
    }
    if (arm9 && !code && cp15_.dtcm_enabled() && address - cp15_.dtcm_base() < cp15_.dtcm_size()) {
        return 1u;
    }
    return RegionCycles(arm9, address >> 24u, width == 4u, sequential, code);
}


void MemoryBus::RefreshAccessTimes() {
    // Index 0Fh stands for the BIOS at FFFF0000h (the table uses address bits 24-27).
    for (uint32_t region = 0u; region < 16u; ++region) {
        for (int code = 0; code < 2; ++code) {
            for (int sequential = 0; sequential < 2; ++sequential) {
                for (int wide = 0; wide < 2; ++wide) {
                    const uint32_t full_region = region == 0xFu ? 0xFFu : region;
                    arm9_times_.cost[region][code][sequential][wide] = static_cast<uint8_t>(
                        RegionCycles(true, full_region, wide != 0, sequential != 0, code != 0));
                    arm7_times_.cost[region][code][sequential][wide] = static_cast<uint8_t>(
                        RegionCycles(false, full_region, wide != 0, sequential != 0, code != 0));
                }
            }
        }
    }
    arm9_times_.itcm_end = cp15_.itcm_enabled() ? static_cast<uint32_t>(std::min<uint64_t>(cp15_.itcm_size(), 0xFFFFFFFFu)) : 0u;
    arm9_times_.dtcm_base = cp15_.dtcm_base();
    arm9_times_.dtcm_size = cp15_.dtcm_enabled() ? static_cast<uint32_t>(std::min<uint64_t>(cp15_.dtcm_size(), 0xFFFFFFFFu)) : 0u;
}


uint32_t MemoryBus::RegionCycles(bool arm9, uint32_t region, bool wide, bool sequential, bool code) const {
    // 8-bit accesses time like 16-bit ones.
    if (!arm9) {
        // NDS7 table (33 MHz units). Main RAM always pays the 3-wait nonsequential penalty.
        switch (region) {
        case 0x02u: return sequential ? (wide ? 2u : 1u) : (wide ? (code ? 9u : 10u) : (code ? 8u : 9u));
        case 0x05u:
        case 0x06u: return wide ? 2u : 1u;
        case 0x08u:
        case 0x09u: return sequential ? (wide ? 12u : 6u) : (wide ? (code ? 16u : 15u) : (code ? 10u : 9u));
        case 0x0Au: return sequential ? 10u : 9u;
        default: return 1u; // BIOS, WRAM, I/O, OAM
        }
    }
    // NDS9: cache hits run at the full 66 MHz.
    // shortcut: no cache is modelled; with a cache enabled, main RAM (and BIOS code) count as hits.
    const uint32_t control = cp15_.control();
    const bool icache = (control & (1u << 12)) != 0u;
    const bool dcache = (control & (1u << 2)) != 0u;
    if (code ? icache && (region == 0x02u || region == 0xFFu) : dcache && region == 0x02u) {
        return 1u;
    }
    if (code) {
        // Opcode fetches are always nonsequential 32-bit with the 3-wait penalty (a Thumb opcode costs half).
        uint32_t bus = 4u; // WRAM, BIOS, I/O, OAM
        switch (region) {
        case 0x02u: bus = 9u; break;
        case 0x05u:
        case 0x06u: bus = 5u; break;
        case 0x08u:
        case 0x09u: bus = 19u; break;
        default: break;
        }
        return wide ? bus * 2u : bus; // ARM9 cycles: x2 for 32 bits, x1 for half of a 32-bit fetch
    }
    uint32_t bus = sequential ? 1u : 4u; // WRAM, BIOS, I/O, OAM
    switch (region) {
    case 0x02u: bus = sequential ? (wide ? 2u : 1u) : (wide ? 10u : 9u); break;
    case 0x05u:
    case 0x06u: bus = sequential ? (wide ? 2u : 1u) : (wide ? 5u : 4u); break;
    case 0x08u:
    case 0x09u: bus = sequential ? (wide ? 12u : 6u) : (wide ? 19u : 13u); break;
    case 0x0Au: bus = sequential ? 10u : 13u; break;
    default: break;
    }
    return bus * 2u;
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
        // Little-endian host; an aligned access stays inside its (word-multiple) backing store.
        uint32_t value = 0u;
        std::memcpy(&value, bytes, size);
        return value;
    }

    if ((address >> 24u) == kSlot2RamRegion) {
        // GBA slot SRAM/FLASH, an 8-bit bus repeated every 64 KiB: the byte fills every lane. The CPU
        // without the slot reads 00h; an empty slot reads FFh (GBATEK "GBA Slot").
        // shortcut: an owned slot without a cartridge reads ROM as 00h, not the Addr/2 open-bus pattern.
        if (self.io_.Slot2Owner() != (arm9 ? 0 : 1)) {
            return 0u;
        }
        const uint8_t byte = slot2_rom_.empty() ? 0xFFu : slot2_backup_.Read8(address & 0xFFFFu);
        return byte * (size == 1u ? 1u : size == 2u ? 0x0101u : 0x01010101u);
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
        std::memcpy(bytes, &value, size);
        return;
    }

    if ((address >> 24u) == kSlot2RamRegion) {
        if (io_.Slot2Owner() == (arm9 ? 0 : 1) && !slot2_rom_.empty()) {
            slot2_backup_.Write8(address & 0xFFFFu, static_cast<uint8_t>(value)); // one byte reaches the 8-bit bus
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
