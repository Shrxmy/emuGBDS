#include "emugbds/inds/Engine2d.h"

#include <algorithm>

namespace emugbds::inds {
namespace {

using Area = VideoMemory::Area;

constexpr uint32_t kBgCnt = 0x08u;
constexpr uint32_t kBgOfs = 0x10u;
constexpr uint32_t kAffine = 0x20u; // BG2; BG3 at +10h
constexpr uint32_t kWin0H = 0x40u;
constexpr uint32_t kWinIn = 0x48u;
constexpr uint32_t kWinOut = 0x4Au;
constexpr uint32_t kBldCnt = 0x50u;
constexpr uint32_t kBldAlpha = 0x52u;
constexpr uint32_t kBldY = 0x54u;
constexpr uint32_t kMasterBright = 0x6Cu;
constexpr uint32_t kLayerObj = 4u;
constexpr uint32_t kLayerBackdrop = 5u;
constexpr uint8_t kAllLayers = 0x3Fu;

// Affine BG kinds.
enum : uint32_t { kAffine8, kAffine16, kBitmap256, kBitmapDirect, kLargeBitmap };

constexpr std::array<std::array<uint8_t, 2>, 12> kObjSizes = {{
    {{8, 8}}, {{16, 16}}, {{32, 32}}, {{64, 64}},
    {{16, 8}}, {{32, 8}}, {{32, 16}}, {{64, 32}},
    {{8, 16}}, {{8, 32}}, {{16, 32}}, {{32, 64}},
}};

int32_t SignExtend28(uint32_t value) noexcept {
    return static_cast<int32_t>(value << 4u) >> 4u;
}

// 5-bit channel blend: (a*eva + b*evb) / 16, saturated.
uint16_t Blend(uint16_t a, uint16_t b, uint32_t eva, uint32_t evb) noexcept {
    uint16_t out = 0u;
    for (uint32_t shift = 0u; shift < 15u; shift += 5u) {
        const uint32_t channel = (((a >> shift) & 31u) * eva + ((b >> shift) & 31u) * evb) / 16u;
        out = static_cast<uint16_t>(out | (std::min(channel, 31u) << shift));
    }
    return out;
}

uint16_t Brighten(uint16_t color, uint32_t factor) noexcept {
    uint16_t out = 0u;
    for (uint32_t shift = 0u; shift < 15u; shift += 5u) {
        const uint32_t channel = (color >> shift) & 31u;
        out = static_cast<uint16_t>(out | ((channel + (31u - channel) * factor / 16u) << shift));
    }
    return out;
}

uint16_t Darken(uint16_t color, uint32_t factor) noexcept {
    uint16_t out = 0u;
    for (uint32_t shift = 0u; shift < 15u; shift += 5u) {
        const uint32_t channel = (color >> shift) & 31u;
        out = static_cast<uint16_t>(out | ((channel - channel * factor / 16u) << shift));
    }
    return out;
}

} // namespace

void Engine2d::Reset() noexcept {
    regs_ = {};
    ref_x_ = {};
    ref_y_ = {};
}


uint32_t Engine2d::ReadWord(uint32_t offset) const noexcept {
    if (offset + 4u > kRegisterBytes) {
        return 0u;
    }
    switch (offset) {
    case 0x00u: // DISPCNT
    case 0x08u: // BG0CNT, BG1CNT
    case 0x0Cu: // BG2CNT, BG3CNT
    case 0x48u: // WININ, WINOUT
    case 0x50u: // BLDCNT, BLDALPHA
    case 0x64u: // DISPCAPCNT
        return Reg32(offset);
    case 0x6Cu:
        return Reg16(offset); // MASTER_BRIGHT
    default:
        return 0u; // Scroll, affine, window size, mosaic, and BLDY are write-only.
    }
}


void Engine2d::WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept {
    if (offset + 4u > kRegisterBytes) {
        return;
    }
    for (uint32_t lane = 0u; lane < 4u; ++lane) {
        if ((mask & (0xFFu << (lane * 8u))) != 0u) {
            regs_[offset + lane] = static_cast<uint8_t>(value >> (lane * 8u));
        }
    }
    // Writing a reference point reloads the internal one at once.
    for (uint32_t i = 0u; i < 2u; ++i) {
        const uint32_t base = kAffine + i * 0x10u;
        if (offset == base + 8u) {
            ref_x_[i] = SignExtend28(Reg32(base + 8u));
        } else if (offset == base + 12u) {
            ref_y_[i] = SignExtend28(Reg32(base + 12u));
        }
    }
}


void Engine2d::RenderLine(uint32_t y, bool enabled, uint32_t* out) noexcept {
    if (y == 0u) {
        for (uint32_t i = 0u; i < 2u; ++i) {
            ref_x_[i] = SignExtend28(Reg32(kAffine + i * 0x10u + 8u));
            ref_y_[i] = SignExtend28(Reg32(kAffine + i * 0x10u + 12u));
        }
    }
    DrawLine(y, enabled, out);
    for (uint32_t i = 0u; i < 2u; ++i) { // Affine references step every line, drawn or not.
        ref_x_[i] += static_cast<int16_t>(Reg16(kAffine + i * 0x10u + 2u));
        ref_y_[i] += static_cast<int16_t>(Reg16(kAffine + i * 0x10u + 6u));
    }
}


void Engine2d::DrawLine(uint32_t y, bool enabled, uint32_t* out) noexcept {
    const uint32_t control = dispcnt() & ~(static_cast<uint32_t>(hidden_layers_) << 8u);
    uint32_t display_mode = (control >> 16u) & 3u;
    if (engine_ == 1) {
        display_mode &= 1u;
    }
    if (!enabled || display_mode == 0u) {
        std::fill(out, out + kWidth, 0xFFFFFFFFu); // Display off is white.
        return;
    }
    if (display_mode == 2u) {
        const uint32_t bank = (control >> 18u) & 3u;
        for (uint32_t x = 0u; x < kWidth; ++x) {
            out[x] = ToAbgr(memory_.LcdcBank16(bank, y * kWidth + x), Reg16(kMasterBright));
        }
        return;
    }
    if (display_mode == 3u) {
        std::fill(out, out + kWidth, ToAbgr(0u, Reg16(kMasterBright))); // shortcut: main-memory display FIFO not emulated.
        return;
    }
    if ((control & (1u << 7)) != 0u) {
        std::fill(out, out + kWidth, 0xFFFFFFFFu); // Forced blank
        return;
    }

    const uint32_t mode = control & 7u;
    bg_active_ = {};
    for (uint32_t bg = 0u; bg < 4u; ++bg) {
        if ((control & (1u << (8u + bg))) == 0u) {
            continue;
        }
        auto& line = bg_line_[bg];
        if (bg == 0u && engine_ == 0 && ((control & (1u << 3)) != 0u || mode == 6u)) {
            continue; // 3D layer (not emulated)
        }
        const uint16_t bgcnt = Reg16(kBgCnt + bg * 2u);
        const uint32_t extended = (bgcnt & 0x80u) == 0u ? kAffine16 : (bgcnt & 4u) != 0u ? kBitmapDirect : kBitmap256;
        bool affine = false;
        uint32_t kind = kAffine8;
        if (bg == 2u) {
            affine = mode == 2u || mode == 4u || mode == 5u || mode == 6u;
            kind = mode == 5u ? extended : mode == 6u ? kLargeBitmap : kAffine8;
            if (engine_ == 1 && mode == 6u) {
                continue;
            }
        } else if (bg == 3u) {
            affine = mode >= 1u && mode <= 5u;
            kind = mode >= 3u ? extended : kAffine8;
            if (mode == 6u) {
                continue;
            }
        } else if (bg == 1u && mode == 6u) {
            continue;
        }
        if (mode == 7u) {
            continue;
        }
        if (affine) {
            RenderAffineBg(bg, kind, line);
        } else {
            RenderTextBg(bg, y, line);
        }
        bg_active_[bg] = true;
    }
    obj_line_.fill(ObjPixel{});
    obj_window_.fill(false);
    if ((control & (1u << 12)) != 0u) {
        RenderObjs(y);
    }
    const LineSetup setup = SetupLine();
    const uint16_t bright = Reg16(kMasterBright);
    for (uint32_t x = 0u; x < kWidth; ++x) {
        out[x] = ToAbgr(Compose(x, y, setup), bright);
    }
}


void Engine2d::RenderTextBg(uint32_t bg, uint32_t y, std::array<int32_t, kWidth>& line) const noexcept {
    const uint32_t control = dispcnt();
    const uint16_t bgcnt = Reg16(kBgCnt + bg * 2u);
    const uint32_t hofs = Reg16(kBgOfs + bg * 4u) & 0x1FFu;
    const uint32_t vofs = Reg16(kBgOfs + bg * 4u + 2u) & 0x1FFu;
    const uint32_t width = (bgcnt & 0x4000u) != 0u ? 512u : 256u;
    const uint32_t height = (bgcnt & 0x8000u) != 0u ? 512u : 256u;
    const uint32_t extra = engine_ == 0 ? 1u : 0u;
    const uint32_t map_base = ((bgcnt >> 8u) & 31u) * 0x800u + extra * ((control >> 27u) & 7u) * 0x10000u;
    const uint32_t char_base = ((bgcnt >> 2u) & 15u) * 0x4000u + extra * ((control >> 24u) & 7u) * 0x10000u;
    const bool colors256 = (bgcnt & 0x80u) != 0u;
    const bool ext_palette = colors256 && (control & (1u << 30)) != 0u;
    const uint32_t slot = bg >= 2u ? bg : bg + ((bgcnt & 0x2000u) != 0u ? 2u : 0u);
    const uint32_t sy = (y + vofs) & (height - 1u);
    const Area area = engine_ == 0 ? Area::EngineABg : Area::EngineBBg;

    // One map entry and one tile-row lookup per tile, not per pixel (tiles never cross a 16 KiB page).
    const uint32_t tile_bytes = colors256 ? 64u : 32u;
    const uint32_t row_bytes = colors256 ? 8u : 4u;
    for (uint32_t x = 0u; x < kWidth;) {
        const uint32_t sx = (x + hofs) & (width - 1u);
        const uint32_t block = (sx >> 8u) + (sy >> 8u) * (width >> 8u);
        const uint32_t entry_address = map_base + block * 0x800u + ((sy & 255u) >> 3u) * 64u + ((sx & 255u) >> 3u) * 2u;
        const uint16_t entry = memory_.Read16(area, entry_address);
        const uint32_t tile = entry & 0x3FFu;
        const bool hflip = (entry & 0x400u) != 0u;
        const uint32_t py = (entry & 0x800u) != 0u ? 7u - (sy & 7u) : sy & 7u;
        const uint32_t palette = entry >> 12u;
        const uint8_t* row = memory_.Row(area, char_base + tile * tile_bytes + py * row_bytes);
        for (uint32_t column = sx & 7u; column < 8u && x < kWidth; ++column, ++x) {
            const uint32_t px = hflip ? 7u - column : column;
            if (colors256) {
                const uint8_t index = row != nullptr ? row[px] : 0u;
                line[x] = index == 0u ? -1
                    : ext_palette ? memory_.BgExtPalette(engine_, slot, palette * 256u + index) & 0x7FFF
                    : memory_.Palette(engine_, false, index) & 0x7FFF;
            } else {
                const uint8_t pair = row != nullptr ? row[px / 2u] : 0u;
                const uint32_t index = (px & 1u) != 0u ? pair >> 4u : pair & 15u;
                line[x] = index == 0u ? -1 : memory_.Palette(engine_, false, palette * 16u + index) & 0x7FFF;
            }
        }
    }
}


void Engine2d::RenderAffineBg(uint32_t bg, uint32_t kind, std::array<int32_t, kWidth>& line) const noexcept {
    const uint32_t control = dispcnt();
    const uint16_t bgcnt = Reg16(kBgCnt + bg * 2u);
    const uint32_t affine = kAffine + (bg - 2u) * 0x10u;
    const int32_t pa = static_cast<int16_t>(Reg16(affine));
    const int32_t pc = static_cast<int16_t>(Reg16(affine + 4u));
    const uint32_t size = (bgcnt >> 14u) & 3u;
    const bool wrap = (bgcnt & 0x2000u) != 0u;
    const Area area = engine_ == 0 ? Area::EngineABg : Area::EngineBBg;
    const uint32_t extra = engine_ == 0 ? 1u : 0u;
    const uint32_t map_base = ((bgcnt >> 8u) & 31u) * 0x800u + extra * ((control >> 27u) & 7u) * 0x10000u;
    const uint32_t char_base = ((bgcnt >> 2u) & 15u) * 0x4000u + extra * ((control >> 24u) & 7u) * 0x10000u;
    const uint32_t bitmap_base = ((bgcnt >> 8u) & 31u) * 0x4000u;

    uint32_t width = 128u << size;
    uint32_t height = width;
    if (kind == kBitmap256 || kind == kBitmapDirect) {
        static constexpr std::array<std::array<uint32_t, 2>, 4> kBitmapSizes = {{{{128u, 128u}}, {{256u, 256u}}, {{512u, 256u}}, {{512u, 512u}}}};
        width = kBitmapSizes[size][0];
        height = kBitmapSizes[size][1];
    } else if (kind == kLargeBitmap) {
        width = (size & 1u) != 0u ? 1024u : 512u;
        height = (size & 1u) != 0u ? 512u : 1024u;
    }

    int32_t tx = ref_x_[bg - 2u];
    int32_t ty = ref_y_[bg - 2u];
    for (uint32_t x = 0u; x < kWidth; ++x, tx += pa, ty += pc) {
        int32_t u = tx >> 8;
        int32_t v = ty >> 8;
        if (wrap) {
            u &= static_cast<int32_t>(width - 1u);
            v &= static_cast<int32_t>(height - 1u);
        } else if (u < 0 || v < 0 || u >= static_cast<int32_t>(width) || v >= static_cast<int32_t>(height)) {
            line[x] = -1;
            continue;
        }
        const uint32_t uu = static_cast<uint32_t>(u);
        const uint32_t vv = static_cast<uint32_t>(v);
        switch (kind) {
        case kAffine8: {
            const uint8_t tile = memory_.Read8(area, map_base + (vv / 8u) * (width / 8u) + uu / 8u);
            const uint8_t index = memory_.Read8(area, char_base + tile * 64u + (vv & 7u) * 8u + (uu & 7u));
            line[x] = index == 0u ? -1 : memory_.Palette(engine_, false, index) & 0x7FFF;
            break;
        }
        case kAffine16: {
            const uint16_t entry = memory_.Read16(area, map_base + ((vv / 8u) * (width / 8u) + uu / 8u) * 2u);
            const uint32_t px = (entry & 0x400u) != 0u ? 7u - (uu & 7u) : uu & 7u;
            const uint32_t py = (entry & 0x800u) != 0u ? 7u - (vv & 7u) : vv & 7u;
            const uint8_t index = memory_.Read8(area, char_base + (entry & 0x3FFu) * 64u + py * 8u + px);
            line[x] = index == 0u ? -1
                : (control & (1u << 30)) != 0u ? memory_.BgExtPalette(engine_, bg, (entry >> 12u) * 256u + index) & 0x7FFF
                : memory_.Palette(engine_, false, index) & 0x7FFF;
            break;
        }
        case kBitmap256:
        case kLargeBitmap: {
            const uint32_t base = kind == kLargeBitmap ? 0u : bitmap_base;
            const uint8_t index = memory_.Read8(area, base + vv * width + uu);
            line[x] = index == 0u ? -1 : memory_.Palette(engine_, false, index) & 0x7FFF;
            break;
        }
        default: { // Direct colour: bit 15 is the alpha (opaque) flag.
            const uint16_t color = memory_.Read16(area, bitmap_base + (vv * width + uu) * 2u);
            line[x] = (color & 0x8000u) != 0u ? color & 0x7FFF : -1;
            break;
        }
        }
    }
}


void Engine2d::RenderObjs(uint32_t y) noexcept {
    const uint32_t control = dispcnt();
    const uint8_t* oam = memory_.oam(engine_);
    const Area area = engine_ == 0 ? Area::EngineAObj : Area::EngineBObj;
    const auto oam16 = [&](uint32_t offset) { return static_cast<uint16_t>(oam[offset] | (oam[offset + 1u] << 8u)); };

    for (uint32_t i = 0u; i < 128u; ++i) {
        const uint16_t attr0 = oam16(i * 8u);
        const uint16_t attr1 = oam16(i * 8u + 2u);
        const uint16_t attr2 = oam16(i * 8u + 4u);
        const bool affine = (attr0 & 0x100u) != 0u;
        if (!affine && (attr0 & 0x200u) != 0u) {
            continue; // Disabled
        }
        const uint32_t shape = attr0 >> 14u;
        if (shape == 3u) {
            continue;
        }
        const uint32_t mode = (attr0 >> 10u) & 3u;
        const uint32_t width = kObjSizes[shape * 4u + (attr1 >> 14u)][0];
        const uint32_t height = kObjSizes[shape * 4u + (attr1 >> 14u)][1];
        const bool double_size = affine && (attr0 & 0x200u) != 0u;
        const uint32_t box_w = double_size ? width * 2u : width;
        const uint32_t box_h = double_size ? height * 2u : height;
        const uint32_t dy = (y - (attr0 & 0xFFu)) & 0xFFu; // wraps at both screen edges
        if (dy >= box_h) {
            continue;
        }
        int32_t obj_x = attr1 & 0x1FFu;
        if (obj_x >= 256) {
            obj_x -= 512;
        }
        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const uint32_t group = ((attr1 >> 9u) & 31u) * 32u;
            pa = static_cast<int16_t>(oam16(group + 6u));
            pb = static_cast<int16_t>(oam16(group + 14u));
            pc = static_cast<int16_t>(oam16(group + 22u));
            pd = static_cast<int16_t>(oam16(group + 30u));
        }
        const bool colors256 = (attr0 & 0x2000u) != 0u;
        const uint32_t tile = attr2 & 0x3FFu;
        const uint32_t priority = (attr2 >> 10u) & 3u;
        const uint32_t palette = attr2 >> 12u;

        for (uint32_t sx = 0u; sx < box_w; ++sx) {
            const int32_t screen_x = obj_x + static_cast<int32_t>(sx);
            if (screen_x < 0 || screen_x >= static_cast<int32_t>(kWidth)) {
                continue;
            }
            int32_t u = 0;
            int32_t v = 0;
            if (affine) {
                const int32_t cx = static_cast<int32_t>(sx) - static_cast<int32_t>(box_w / 2u);
                const int32_t cy = static_cast<int32_t>(dy) - static_cast<int32_t>(box_h / 2u);
                u = ((pa * cx + pb * cy) >> 8) + static_cast<int32_t>(width / 2u);
                v = ((pc * cx + pd * cy) >> 8) + static_cast<int32_t>(height / 2u);
                if (u < 0 || v < 0 || u >= static_cast<int32_t>(width) || v >= static_cast<int32_t>(height)) {
                    continue;
                }
            } else {
                u = (attr1 & 0x1000u) != 0u ? static_cast<int32_t>(width - 1u - sx) : static_cast<int32_t>(sx);
                v = (attr1 & 0x2000u) != 0u ? static_cast<int32_t>(height - 1u - dy) : static_cast<int32_t>(dy);
            }
            const uint32_t uu = static_cast<uint32_t>(u);
            const uint32_t vv = static_cast<uint32_t>(v);

            int32_t color = -1;
            if (mode == 3u) { // Bitmap OBJ: direct colour with alpha flag
                uint32_t address = 0u;
                if ((control & (1u << 6)) != 0u) {
                    const uint32_t boundary = engine_ == 0 && (control & (1u << 22)) != 0u ? 256u : 128u;
                    address = tile * boundary + (vv * width + uu) * 2u;
                } else {
                    const uint32_t mask_x = (control & (1u << 5)) != 0u ? 0x1Fu : 0x0Fu;
                    const uint32_t row_bytes = (control & (1u << 5)) != 0u ? 512u : 256u;
                    address = (tile & mask_x) * 0x10u + (tile & ~mask_x) * 0x80u + vv * row_bytes + uu * 2u;
                }
                const uint16_t pixel = memory_.Read16(area, address);
                color = (pixel & 0x8000u) != 0u ? pixel & 0x7FFF : -1;
            } else {
                const uint32_t tile_bytes = colors256 ? 64u : 32u;
                uint32_t address = 0u;
                if ((control & (1u << 4)) != 0u) { // 1D mapping
                    const uint32_t boundary = 32u << ((control >> 20u) & 3u);
                    address = tile * boundary + ((vv / 8u) * (width / 8u) + uu / 8u) * tile_bytes;
                } else {
                    // 2D: 32 tiles per row; 256-colour OBJs ignore tile bit 0.
                    address = (colors256 ? tile & ~1u : tile) * 32u + (vv / 8u) * 1024u + (uu / 8u) * tile_bytes;
                }
                if (colors256) {
                    const uint8_t index = memory_.Read8(area, address + (vv & 7u) * 8u + (uu & 7u));
                    if (index != 0u) {
                        color = ((control & (1u << 31)) != 0u ? memory_.ObjExtPalette(engine_, palette * 256u + index)
                                                                : memory_.Palette(engine_, true, index)) & 0x7FFF;
                    }
                } else {
                    const uint8_t pair = memory_.Read8(area, address + (vv & 7u) * 4u + (uu & 7u) / 2u);
                    const uint32_t index = (uu & 1u) != 0u ? pair >> 4u : pair & 15u;
                    if (index != 0u) {
                        color = memory_.Palette(engine_, true, palette * 16u + index) & 0x7FFF;
                    }
                }
            }
            if (color < 0) {
                continue;
            }
            const uint32_t x = static_cast<uint32_t>(screen_x);
            if (mode == 2u) {
                obj_window_[x] = true;
                continue;
            }
            ObjPixel& pixel = obj_line_[x];
            if (pixel.color < 0 || priority < pixel.priority) { // lower OAM index wins ties
                pixel.color = color;
                pixel.priority = static_cast<uint8_t>(priority);
                pixel.blend = mode == 1u || mode == 3u;
                // shortcut: GBATEK leaves the Alpha-OAM scale undefined; bitmap OBJs blend with EVA = alpha + 1.
                pixel.alpha = static_cast<uint8_t>(mode == 3u ? palette + 1u : 0u);
            }
        }
    }
}


uint8_t Engine2d::WindowMask(uint32_t x, uint32_t y) const noexcept {
    const uint32_t control = dispcnt();
    if ((control & 0xE000u) == 0u) {
        return kAllLayers;
    }
    const auto inside = [](uint32_t value, uint32_t range) {
        const uint32_t end = range & 0xFFu;
        const uint32_t start = range >> 8u;
        return start <= end ? value >= start && value < end : value >= start || value < end;
    };
    for (uint32_t window = 0u; window < 2u; ++window) {
        if ((control & (1u << (13u + window))) != 0u &&
            inside(x, Reg16(kWin0H + window * 2u)) && inside(y, Reg16(kWin0H + 4u + window * 2u))) {
            return static_cast<uint8_t>((Reg16(kWinIn) >> (window * 8u)) & kAllLayers);
        }
    }
    if ((control & (1u << 15)) != 0u && obj_window_[x]) {
        return static_cast<uint8_t>((Reg16(kWinOut) >> 8u) & kAllLayers);
    }
    return static_cast<uint8_t>(Reg16(kWinOut) & kAllLayers);
}


Engine2d::LineSetup Engine2d::SetupLine() const noexcept {
    LineSetup setup;
    setup.backdrop = static_cast<uint16_t>(memory_.Palette(engine_, false, 0u) & 0x7FFFu);
    setup.bldcnt = Reg16(kBldCnt);
    const uint16_t bldalpha = Reg16(kBldAlpha);
    setup.eva = std::min<uint32_t>(bldalpha & 31u, 16u);
    setup.evb = std::min<uint32_t>((bldalpha >> 8u) & 31u, 16u);
    setup.evy = std::min<uint32_t>(Reg16(kBldY) & 31u, 16u);
    setup.windows = (dispcnt() & 0xE000u) != 0u;
    for (uint32_t priority = 0u; priority < 4u; ++priority) {
        for (uint32_t bg = 0u; bg < 4u; ++bg) {
            if (bg_active_[bg] && (Reg16(kBgCnt + bg * 2u) & 3u) == priority) {
                setup.bgs[setup.bg_count] = static_cast<uint8_t>(bg);
                setup.bg_priority[setup.bg_count] = static_cast<uint8_t>(priority);
                ++setup.bg_count;
            }
        }
    }
    return setup;
}


uint16_t Engine2d::Compose(uint32_t x, uint32_t y, const LineSetup& setup) const noexcept {
    const uint8_t window = setup.windows ? WindowMask(x, y) : kAllLayers;
    // Top two visible layers by priority; OBJs beat BGs of equal priority.
    uint32_t layers[2] = {kLayerBackdrop, kLayerBackdrop};
    uint16_t colors[2] = {setup.backdrop, setup.backdrop};
    uint32_t found = 0u;
    const ObjPixel& obj = obj_line_[x];
    bool obj_pending = obj.color >= 0 && (window & (1u << kLayerObj)) != 0u;
    for (uint32_t i = 0u; i < setup.bg_count && found < 2u; ++i) {
        if (obj_pending && obj.priority <= setup.bg_priority[i]) {
            layers[found] = kLayerObj;
            colors[found] = static_cast<uint16_t>(obj.color);
            ++found;
            obj_pending = false;
            if (found == 2u) {
                break;
            }
        }
        const uint32_t bg = setup.bgs[i];
        if (bg_line_[bg][x] >= 0 && (window & (1u << bg)) != 0u) {
            layers[found] = bg;
            colors[found] = static_cast<uint16_t>(bg_line_[bg][x]);
            ++found;
        }
    }
    if (obj_pending && found < 2u) {
        layers[found] = kLayerObj;
        colors[found] = static_cast<uint16_t>(obj.color);
        ++found;
    }

    const uint16_t bldcnt = setup.bldcnt;
    const bool effects = (window & 0x20u) != 0u;
    const bool second_target = (bldcnt & (1u << (8u + layers[1]))) != 0u;
    uint16_t color = colors[0];
    if (layers[0] == kLayerObj && obj.blend && second_target) {
        color = obj.alpha != 0u ? Blend(colors[0], colors[1], obj.alpha, 16u - obj.alpha)
                                : Blend(colors[0], colors[1], setup.eva, setup.evb);
    } else if (effects && (bldcnt & (1u << layers[0])) != 0u) {
        switch ((bldcnt >> 6u) & 3u) {
        case 1u:
            if (second_target) {
                color = Blend(colors[0], colors[1], setup.eva, setup.evb);
            }
            break;
        case 2u: color = Brighten(color, setup.evy); break;
        case 3u: color = Darken(color, setup.evy); break;
        default: break;
        }
    }
    return color;
}


uint32_t Engine2d::ToAbgr(uint16_t color, uint16_t bright) noexcept {
    const uint32_t factor = std::min<uint32_t>(bright & 31u, 16u);
    switch (bright >> 14u) {
    case 1u: color = Brighten(color, factor); break;
    case 2u: color = Darken(color, factor); break;
    default: break;
    }
    const auto expand = [](uint32_t channel) { return (channel << 3u) | (channel >> 2u); };
    return 0xFF000000u | (expand((color >> 10u) & 31u) << 16u) | (expand((color >> 5u) & 31u) << 8u) | expand(color & 31u);
}

} // namespace emugbds::inds
