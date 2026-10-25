#pragma once

// DS video memory: the nine VRAM banks (A-I) mapped by VRAMCNT, the standard
// palettes (5000000h) and OAM (7000000h). Banks are mapped in 16 KiB pages into
// the ARM9 LCDC, engine A/B BG and OBJ areas, the ARM7 area, and the extended
// palette slots used by the 2D engines (GBATEK "DS Memory Control - VRAM").
// shortcut: when two banks overlap in one area, the higher bank wins instead of
// reads being ORed together; real software avoids overlaps.

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::inds {

class VideoMemory {
public:
    static constexpr uint32_t kPageSize = 0x4000u;
    enum class Area : uint8_t { Lcdc, EngineABg, EngineAObj, EngineBBg, EngineBObj, Arm7, Count };

    VideoMemory();
    void Reset();
    // VRAMCNT_A..I (bank 0..8).
    void SetBankControl(uint32_t bank, uint8_t value);
    uint8_t bank_control(uint32_t bank) const noexcept { return control_[bank]; }
    // VRAMSTAT bits 0-1: banks C and D allocated to the ARM7.
    uint8_t vramstat() const noexcept;

    // CPU view of 05xxxxxxh, 06xxxxxxh, or 07xxxxxxh; nullptr when unmapped.
    uint8_t* CpuPointer(bool arm9, uint32_t address) noexcept;

    // Display-side reads. Offsets are relative to the area base; unmapped reads are 0.
    uint8_t Read8(Area area, uint32_t offset) const noexcept;
    // `offset` in place, for reading a run of bytes inside one 16 KiB page (a tile row); nullptr when unmapped.
    const uint8_t* Row(Area area, uint32_t offset) const noexcept;
    uint16_t Read16(Area area, uint32_t offset) const noexcept {
        return static_cast<uint16_t>(Read8(area, offset) | (Read8(area, offset + 1u) << 8u));
    }
    // Extended palettes: engine 0/1, slot 0-3 for BGs (8 KiB each); OBJ uses slot 0.
    uint16_t BgExtPalette(int engine, uint32_t slot, uint32_t index) const noexcept;
    uint16_t ObjExtPalette(int engine, uint32_t index) const noexcept;
    // Standard palettes: engine 0/1, BG then OBJ (256 colours each).
    uint16_t Palette(int engine, bool obj, uint32_t index) const noexcept;
    const uint8_t* oam(int engine) const noexcept { return &oam_[engine * 0x400u]; }
    // 16-bit pixel `pixel` of a bank in LCDC mode (VRAM display mode); 0 otherwise.
    uint16_t LcdcBank16(uint32_t bank, uint32_t pixel) const noexcept;

private:
    void Remap();
    void MapPages(Area area, uint32_t first_page, uint32_t bank);

    std::array<std::vector<uint8_t>, 9> banks_;
    std::array<uint8_t, 9> control_{};
    std::array<std::array<uint8_t*, 41>, static_cast<size_t>(Area::Count)> pages_{};
    std::array<std::array<const uint8_t*, 4>, 2> bg_ext_{};  // 8 KiB slots
    std::array<const uint8_t*, 2> obj_ext_{};                // 8 KiB
    std::vector<uint8_t> palette_;
    std::vector<uint8_t> oam_;
};

} // namespace emugbds::inds
