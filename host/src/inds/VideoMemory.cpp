#include "emugbds/inds/VideoMemory.h"

#include <algorithm>

namespace emugbds::inds {
namespace {

enum Bank : uint32_t { A, B, C, D, E, F, G, H, I };

constexpr std::array<uint32_t, 9> kBankSize = {
    0x20000u, 0x20000u, 0x20000u, 0x20000u, 0x10000u, 0x4000u, 0x4000u, 0x8000u, 0x4000u};
// LCDC (MST 0) addresses relative to 6800000h.
constexpr std::array<uint32_t, 9> kLcdcOffset = {
    0x00000u, 0x20000u, 0x40000u, 0x60000u, 0x80000u, 0x90000u, 0x94000u, 0x98000u, 0xA0000u};
// MST bits each bank decodes (A, B, H, I ignore bit 2).
constexpr std::array<uint8_t, 9> kMstMask = {3u, 3u, 7u, 7u, 7u, 7u, 7u, 3u, 3u};
// Pages per area; CPU addresses mirror within it.
constexpr std::array<uint32_t, 6> kAreaPages = {41u, 32u, 16u, 8u, 8u, 16u};
constexpr uint32_t kExtSlotSize = 0x2000u;

} // namespace

VideoMemory::VideoMemory() : palette_(0x800u, 0u), oam_(0x800u, 0u) {
    for (uint32_t bank = 0u; bank < banks_.size(); ++bank) {
        banks_[bank].assign(kBankSize[bank], 0u);
    }
    Reset();
}


void VideoMemory::Reset() {
    for (auto& bank : banks_) {
        std::fill(bank.begin(), bank.end(), 0u);
    }
    std::fill(palette_.begin(), palette_.end(), 0u);
    std::fill(oam_.begin(), oam_.end(), 0u);
    control_ = {};
    Remap();
}


void VideoMemory::SetBankControl(uint32_t bank, uint8_t value) {
    if (bank < control_.size() && control_[bank] != value) {
        control_[bank] = value;
        Remap();
    }
}


uint8_t VideoMemory::vramstat() const noexcept {
    const auto arm7 = [&](uint32_t bank) { return (control_[bank] & 0x87u) == 0x82u; };
    return static_cast<uint8_t>((arm7(C) ? 1u : 0u) | (arm7(D) ? 2u : 0u));
}


void VideoMemory::MapPages(Area area, uint32_t first_page, uint32_t bank) {
    auto& pages = pages_[static_cast<size_t>(area)];
    const uint32_t count = kBankSize[bank] / kPageSize;
    for (uint32_t page = 0u; page < count && first_page + page < kAreaPages[static_cast<size_t>(area)]; ++page) {
        pages[first_page + page] = &banks_[bank][page * kPageSize];
    }
}


void VideoMemory::Remap() {
    for (auto& area : pages_) {
        area.fill(nullptr);
    }
    for (auto& engine : bg_ext_) {
        engine.fill(nullptr);
    }
    obj_ext_.fill(nullptr);

    for (uint32_t bank = 0u; bank < banks_.size(); ++bank) {
        const uint8_t control = control_[bank];
        if ((control & 0x80u) == 0u) {
            continue;
        }
        const uint32_t mst = control & kMstMask[bank];
        const uint32_t ofs = (control >> 3u) & 3u;
        const uint32_t small_ofs = (ofs & 1u) + (ofs >> 1u) * 4u; // F/G: 4000h*OFS.0 + 10000h*OFS.1, in pages
        const uint8_t* data = banks_[bank].data();
        switch (mst) {
        case 0u:
            MapPages(Area::Lcdc, kLcdcOffset[bank] / kPageSize, bank);
            break;
        case 1u:
            if (bank <= D) {
                MapPages(Area::EngineABg, ofs * 8u, bank);
            } else if (bank == E) {
                MapPages(Area::EngineABg, 0u, bank);
            } else if (bank == F || bank == G) {
                MapPages(Area::EngineABg, small_ofs, bank);
            } else if (bank == H) {
                MapPages(Area::EngineBBg, 0u, bank);
            } else {
                MapPages(Area::EngineBBg, 2u, bank); // I at 6208000h
            }
            break;
        case 2u:
            if (bank == A || bank == B) {
                MapPages(Area::EngineAObj, (ofs & 1u) * 8u, bank);
            } else if (bank == C || bank == D) {
                MapPages(Area::Arm7, (ofs & 1u) * 8u, bank);
            } else if (bank == E) {
                MapPages(Area::EngineAObj, 0u, bank);
            } else if (bank == F || bank == G) {
                MapPages(Area::EngineAObj, small_ofs, bank);
            } else if (bank == H) {
                for (uint32_t slot = 0u; slot < 4u; ++slot) {
                    bg_ext_[1][slot] = data + slot * kExtSlotSize;
                }
            } else {
                MapPages(Area::EngineBObj, 0u, bank); // I
            }
            break;
        case 3u:
            if (bank == I) {
                obj_ext_[1] = data;
            }
            // shortcut: texture image/palette slots (A-G) wait for the 3D engine.
            break;
        case 4u:
            if (bank == C) {
                MapPages(Area::EngineBBg, 0u, bank);
            } else if (bank == D) {
                MapPages(Area::EngineBObj, 0u, bank);
            } else if (bank == E) {
                for (uint32_t slot = 0u; slot < 4u; ++slot) {
                    bg_ext_[0][slot] = data + slot * kExtSlotSize;
                }
            } else if (bank == F || bank == G) {
                const uint32_t first = (ofs & 1u) * 2u;
                bg_ext_[0][first] = data;
                bg_ext_[0][first + 1u] = data + kExtSlotSize;
            }
            break;
        case 5u:
            if (bank == F || bank == G) {
                obj_ext_[0] = data;
            }
            break;
        default:
            break;
        }
    }
}


uint8_t* VideoMemory::CpuPointer(bool arm9, uint32_t address) noexcept {
    switch (address >> 24u) {
    case 0x05u:
        return arm9 ? &palette_[address & 0x7FFu] : nullptr;
    case 0x07u:
        return arm9 ? &oam_[address & 0x7FFu] : nullptr;
    case 0x06u:
        break;
    default:
        return nullptr;
    }
    Area area = Area::Arm7;
    uint32_t page = (address >> 14u) & 0xFu;
    if (arm9) {
        switch ((address >> 21u) & 7u) { // 2 MiB windows
        case 0u: area = Area::EngineABg; page = (address >> 14u) & 31u; break;
        case 1u: area = Area::EngineBBg; page = (address >> 14u) & 7u; break;
        case 2u: area = Area::EngineAObj; page = (address >> 14u) & 15u; break;
        case 3u: area = Area::EngineBObj; page = (address >> 14u) & 7u; break;
        default: area = Area::Lcdc; page = (address >> 14u) & 0x3Fu; break;
        }
        if (page >= kAreaPages[static_cast<size_t>(area)]) {
            return nullptr;
        }
    }
    uint8_t* base = pages_[static_cast<size_t>(area)][page];
    return base != nullptr ? base + (address & (kPageSize - 1u)) : nullptr;
}


uint8_t VideoMemory::Read8(Area area, uint32_t offset) const noexcept {
    const uint8_t* row = Row(area, offset);
    return row != nullptr ? *row : 0u;
}


const uint8_t* VideoMemory::Row(Area area, uint32_t offset) const noexcept {
    const uint32_t pages = kAreaPages[static_cast<size_t>(area)];
    uint32_t page = offset / kPageSize;
    if (page >= pages) {
        page %= pages; // only mirrored offsets pay for the division
    }
    const uint8_t* base = pages_[static_cast<size_t>(area)][page];
    return base != nullptr ? base + (offset & (kPageSize - 1u)) : nullptr;
}


uint16_t VideoMemory::BgExtPalette(int engine, uint32_t slot, uint32_t index) const noexcept {
    const uint8_t* base = bg_ext_[engine][slot & 3u];
    const uint32_t offset = (index & 0xFFFu) * 2u;
    return base != nullptr ? static_cast<uint16_t>(base[offset] | (base[offset + 1u] << 8u)) : 0u;
}


uint16_t VideoMemory::ObjExtPalette(int engine, uint32_t index) const noexcept {
    const uint8_t* base = obj_ext_[engine];
    const uint32_t offset = (index & 0xFFFu) * 2u;
    return base != nullptr ? static_cast<uint16_t>(base[offset] | (base[offset + 1u] << 8u)) : 0u;
}


uint16_t VideoMemory::Palette(int engine, bool obj, uint32_t index) const noexcept {
    const uint32_t offset = static_cast<uint32_t>(engine) * 0x400u + (obj ? 0x200u : 0u) + (index & 0xFFu) * 2u;
    return static_cast<uint16_t>(palette_[offset] | (palette_[offset + 1u] << 8u));
}


uint16_t VideoMemory::LcdcBank16(uint32_t bank, uint32_t pixel) const noexcept {
    if (bank >= banks_.size() || (control_[bank] & (0x80u | kMstMask[bank])) != 0x80u) {
        return 0u; // Only a bank in LCDC mode can be displayed.
    }
    const auto& data = banks_[bank];
    const uint32_t offset = (pixel * 2u) % data.size();
    return static_cast<uint16_t>(data[offset] | (data[offset + 1u] << 8u));
}


void VideoMemory::WriteLcdcBank16(uint32_t bank, uint32_t pixel, uint16_t value) noexcept {
    if (bank >= banks_.size() || (control_[bank] & (0x80u | kMstMask[bank])) != 0x80u) {
        return;
    }
    auto& data = banks_[bank];
    const uint32_t offset = (pixel * 2u) % data.size();
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

} // namespace emugbds::inds
