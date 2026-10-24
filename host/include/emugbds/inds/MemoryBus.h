#pragma once

// DS memory: BIOS and cartridge images, RAM, TCM, and the ARM9/ARM7 bus views
// that map them (GBATEK "DS Memory Maps").

#include "emugbds/arm/Bus.h"
#include "emugbds/inds/Cp15.h"
#include "emugbds/inds/Io.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace emugbds::inds {

// I/O ports and memory regions that are not emulated yet, counted per address
// (I/O) or per 64 KiB block (memory) so bring-up reports show what a game needs.
struct AccessLog {
    // Past this many distinct keys (a runaway program touching everything), new
    // keys are counted under kOverflowKey so the log stays small.
    static constexpr size_t kMaxKeys = 4096u;
    static constexpr uint32_t kOverflowKey = 0xFFFFFFFFu;

    std::map<uint32_t, uint64_t> reads;
    std::map<uint32_t, uint64_t> writes;

    static void Count(std::map<uint32_t, uint64_t>& counts, uint32_t key) {
        const auto it = counts.find(key);
        if (it != counts.end()) {
            ++it->second;
        } else {
            ++counts[counts.size() < kMaxKeys ? key : kOverflowKey];
        }
    }
};

class MemoryBus {
public:
    static constexpr uint32_t kMainRamBase = 0x02000000u;
    static constexpr uint32_t kMainRamSize = 0x00400000u;
    static constexpr uint32_t kSharedWramSize = 0x00008000u;
    static constexpr uint32_t kArm7WramSize = 0x00010000u;
    static constexpr uint32_t kSlot2Base = 0x08000000u;
    static constexpr uint32_t kArm9BiosBase = 0xFFFF0000u;
    static constexpr uint32_t kArm9BiosRegionSize = 0x8000u;

    MemoryBus();
    // The bus views refer back to this object.
    MemoryBus(const MemoryBus&) = delete;
    MemoryBus& operator=(const MemoryBus&) = delete;

    bool LoadBios7(const std::filesystem::path& bios_path, std::string& error);
    bool LoadBios9(const std::filesystem::path& bios_path, std::string& error);
    bool LoadFirmware(const std::filesystem::path& firmware_path, std::string& error);
    bool LoadNdsRom(const std::filesystem::path& rom_path, std::string& error);
    bool LoadSlot2Rom(const std::filesystem::path& rom_path, std::string& error);
    void ClearNdsRom();
    void ClearSlot2Rom();

    bool HasBios7() const noexcept { return !bios7_.empty(); }
    bool HasBios9() const noexcept { return !bios9_.empty(); }
    bool HasFirmware() const noexcept { return !firmware_.empty(); }
    bool HasNdsRom() const noexcept { return !nds_rom_.empty(); }
    bool HasSlot2Rom() const noexcept { return !slot2_rom_.empty(); }
    const std::vector<uint8_t>& nds_rom() const noexcept { return nds_rom_; }
    // The firmware flash contents; SPI writes change this copy, never the file.
    std::vector<uint8_t>& firmware_image() noexcept { return firmware_; }

    // Clears RAM and TCM and restores power-on I/O, WRAMCNT, and CP15; loaded images stay.
    void ResetMemory();
    // Shared WRAM split (0 = all ARM9 ... 3 = all ARM7), as set by WRAMCNT.
    uint8_t wramcnt() const noexcept { return wramcnt_; }
    void set_wramcnt(uint8_t value) noexcept { wramcnt_ = static_cast<uint8_t>(value & 3u); }
    // DMA accesses: same maps as the CPUs, but they cannot reach the ARM9's TCM.
    uint32_t DmaRead(bool arm9, uint32_t address, uint32_t size) { return Read(arm9, address, size, false); }
    void DmaWrite(bool arm9, uint32_t address, uint32_t value, uint32_t size) { Write(arm9, address, value, size, false); }

    // The ARM9's CP15 lives here because its TCM settings decide the ARM9 memory map.
    Cp15& cp15() noexcept { return cp15_; }
    const Cp15& cp15() const noexcept { return cp15_; }
    Io& io() noexcept { return io_; }
    const Io& io() const noexcept { return io_; }
    arm::IBus& arm9() noexcept { return arm9_view_; }
    arm::IBus& arm7() noexcept { return arm7_view_; }
    const AccessLog& arm9_unmapped() const noexcept { return arm9_unmapped_; }
    const AccessLog& arm7_unmapped() const noexcept { return arm7_unmapped_; }

private:
    class View final : public arm::IBus {
    public:
        View(MemoryBus& memory, bool arm9) noexcept : memory_(memory), arm9_(arm9) {}
        uint8_t Read8(uint32_t address) const override { return static_cast<uint8_t>(memory_.Read(arm9_, address, 1u)); }
        uint16_t Read16(uint32_t address) const override { return static_cast<uint16_t>(memory_.Read(arm9_, address, 2u)); }
        uint32_t Read32(uint32_t address) const override { return memory_.Read(arm9_, address, 4u); }
        void Write8(uint32_t address, uint8_t value) override { memory_.Write(arm9_, address, value, 1u); }
        void Write16(uint32_t address, uint16_t value) override { memory_.Write(arm9_, address, value, 2u); }
        void Write32(uint32_t address, uint32_t value) override { memory_.Write(arm9_, address, value, 4u); }

    private:
        MemoryBus& memory_;
        bool arm9_;
    };

    uint32_t Read(bool arm9, uint32_t address, uint32_t size, bool use_tcm = true) const;
    void Write(bool arm9, uint32_t address, uint32_t value, uint32_t size, bool use_tcm = true);
    // Backing storage for an address, or nullptr for I/O and unmapped space.
    uint8_t* Arm9Pointer(uint32_t address, bool write, bool use_tcm);
    uint8_t* Arm7Pointer(uint32_t address, bool write);

    std::vector<uint8_t> bios7_;
    std::vector<uint8_t> bios9_;
    std::vector<uint8_t> firmware_;
    std::vector<uint8_t> main_ram_;
    std::vector<uint8_t> shared_wram_;
    std::vector<uint8_t> arm7_wram_;
    std::vector<uint8_t> itcm_;
    std::vector<uint8_t> dtcm_;
    std::vector<uint8_t> nds_rom_;
    std::vector<uint8_t> slot2_rom_;
    uint8_t wramcnt_ = 3u;
    Cp15 cp15_{};
    Io io_{*this};
    View arm9_view_{*this, true};
    View arm7_view_{*this, false};
    mutable AccessLog arm9_unmapped_;
    mutable AccessLog arm7_unmapped_;
};

} // namespace emugbds::inds
