#include "emugbds/ingba/Core.h"
#include "emugbds/ingba/MemoryConstants.h"
#include "emugbds/ingba/VideoConstants.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace emugbds::ingba {
namespace {

constexpr uint32_t kObjVramBase = 0x06010000u;
constexpr uint32_t kObjOamBase = 0x07000000u;
constexpr uint32_t kWindow0HAddress = 0x04000040u;
constexpr uint32_t kWindow1HAddress = 0x04000042u;
constexpr uint32_t kWindow0VAddress = 0x04000044u;
constexpr uint32_t kWindow1VAddress = 0x04000046u;
constexpr uint32_t kWindowInAddress = 0x04000048u;
constexpr uint32_t kWindowOutAddress = 0x0400004Au;
constexpr uint32_t kMosaicAddress = 0x0400004Cu;
constexpr uint32_t kBlendCntAddress = 0x04000050u;
constexpr uint32_t kBlendAlphaAddress = 0x04000052u;
constexpr uint32_t kBlendYAddress = 0x04000054u;

constexpr std::array<std::array<uint32_t, 2u>, 12u> kObjSizes = {{
    {{8u, 8u}}, {{16u, 16u}}, {{32u, 32u}}, {{64u, 64u}},
    {{16u, 8u}}, {{32u, 8u}}, {{32u, 16u}}, {{64u, 32u}},
    {{8u, 16u}}, {{8u, 32u}}, {{16u, 32u}}, {{32u, 64u}},
}};

uint32_t MixColors(uint32_t first, uint32_t second, uint32_t first_weight, uint32_t second_weight) {
    auto mix_channel = [&](uint32_t shift) {
        const uint32_t a = (first >> shift) & 0xFFu;
        const uint32_t b = (second >> shift) & 0xFFu;
        return std::min(255u, (a * first_weight + b * second_weight) / 16u);
    };

    return 0xFF000000u |
        (mix_channel(16u) << 16u) |
        (mix_channel(8u) << 8u) |
        mix_channel(0u);
}

uint32_t BrightenColor(uint32_t color, uint32_t amount) {
    amount = std::min(amount, 16u);
    auto brighten_channel = [amount](uint32_t channel) {
        return channel + ((255u - channel) * amount) / 16u;
    };
    const uint32_t r = brighten_channel(color & 0xFFu);
    const uint32_t g = brighten_channel((color >> 8u) & 0xFFu);
    const uint32_t b = brighten_channel((color >> 16u) & 0xFFu);
    return 0xFF000000u | (b << 16u) | (g << 8u) | r;
}

uint32_t DarkenColor(uint32_t color, uint32_t amount) {
    amount = std::min(amount, 16u);
    const uint32_t scale = 16u - amount;
    const uint32_t r = ((color & 0xFFu) * scale) / 16u;
    const uint32_t g = (((color >> 8u) & 0xFFu) * scale) / 16u;
    const uint32_t b = (((color >> 16u) & 0xFFu) * scale) / 16u;
    return 0xFF000000u | (b << 16u) | (g << 8u) | r;
}

} // namespace

void Core::AdvanceLcdTiming(uint32_t elapsed_cycles) noexcept {
    const uint16_t vcount = static_cast<uint16_t>((elapsed_cycles / kCyclesPerScanline) % kTotalScanlines);
    const uint32_t line_cycle = elapsed_cycles % kCyclesPerScanline;
    const bool vblank = vcount >= kVisibleScanlines;
    const bool hblank = line_cycle >= 1008u;
    const uint16_t dispstat = bus_.Read16(kDispStatAddress);
    const uint16_t vcount_target = static_cast<uint16_t>((dispstat >> 8u) & 0x00FFu);
    const bool vcounter_match = vcount == vcount_target;
    if (line_cycle == 0u && vcount < kVisibleScanlines) {
        CaptureVideoScanlineState(vcount);
    }

    if (vblank && !in_vblank_) {
        if ((dispstat & kDispStatVBlankIrqEnable) != 0u) {
            bus_.SetInterruptRequest(0x0001u);
        }
        bus_.TriggerDmaTiming(0x1u);
    }
    if (hblank && !in_hblank_) {
        if ((dispstat & kDispStatHBlankIrqEnable) != 0u) {
            bus_.SetInterruptRequest(0x0002u);
        }
        if (!vblank) {
            bus_.TriggerDmaTiming(0x2u);
            if (vcount >= 2u && vcount < (kVisibleScanlines + 2u)) {
                bus_.TriggerDmaTiming(0x3u); // Display-start DMA (DMA3 only).
            }
        }
    }
    if (vcounter_match && !in_vcounter_match_ && (dispstat & kDispStatVCounterIrqEnable) != 0u) {
        bus_.SetInterruptRequest(0x0004u);
    }
    in_vblank_ = vblank;
    in_hblank_ = hblank;
    in_vcounter_match_ = vcounter_match;
    bus_.SetLcdStatus(vcount, vblank, hblank, vcounter_match);
}


void Core::CaptureVideoScanlineState(uint16_t scanline) noexcept {
    if (scanline >= kVideoHeight) {
        return;
    }
    auto& state = video_scanline_states_[scanline];
    state.dispcnt = bus_.Read16(kDispCntAddress);
    for (uint32_t bg = 0u; bg < 4u; ++bg) {
        state.bgcnt[bg] = bus_.Read16(kBgCntAddressBase + bg * 2u);
        state.hofs[bg] = bus_.Read16(kBgHofsAddressBase + bg * 4u);
        state.vofs[bg] = bus_.Read16(kBgVofsAddressBase + bg * 4u);
    }
    for (uint32_t affine = 0u; affine < 2u; ++affine) {
        const uint32_t address = 0x04000020u + affine * 0x10u;
        state.affine_pa[affine + 2u] = static_cast<int16_t>(bus_.Read16(address));
        state.affine_pb[affine + 2u] = static_cast<int16_t>(bus_.Read16(address + 2u));
        state.affine_pc[affine + 2u] = static_cast<int16_t>(bus_.Read16(address + 4u));
        state.affine_pd[affine + 2u] = static_cast<int16_t>(bus_.Read16(address + 6u));
        const auto sign_extend_28 = [](uint32_t value) {
            return static_cast<int32_t>(value << 4u) >> 4u;
        };
        state.reference_x[affine + 2u] = sign_extend_28(bus_.Read32(address + 8u));
        state.reference_y[affine + 2u] = sign_extend_28(bus_.Read32(address + 12u));
    }
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

    std::vector<uint32_t> background_colors(kVideoPixelCount, backdrop);
    std::vector<uint8_t> background_priorities(kVideoPixelCount, 4u);
    std::vector<uint8_t> background_layers(kVideoPixelCount, 4u);
    std::vector<uint32_t> second_background_colors(kVideoPixelCount, backdrop);
    std::vector<uint8_t> second_background_priorities(kVideoPixelCount, 4u);
    std::vector<uint8_t> second_background_layers(kVideoPixelCount, 4u); // Backdrop is the final fallback.

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

    const bool win0_enabled = (dispcnt & (1u << 13u)) != 0u;
    const bool win1_enabled = (dispcnt & (1u << 14u)) != 0u;
    const bool objwin_enabled = (dispcnt & (1u << 15u)) != 0u;
    const bool any_window_enabled = win0_enabled || win1_enabled || objwin_enabled;
    const uint16_t win0_h = bus_.Read16(kWindow0HAddress);
    const uint16_t win1_h = bus_.Read16(kWindow1HAddress);
    const uint16_t win0_v = bus_.Read16(kWindow0VAddress);
    const uint16_t win1_v = bus_.Read16(kWindow1VAddress);
    const uint16_t win_in = bus_.Read16(kWindowInAddress) & 0x3F3Fu;
    const uint16_t win_out = bus_.Read16(kWindowOutAddress) & 0x3F3Fu;
    const auto in_window_axis = [](uint32_t coordinate, uint32_t start, uint32_t end) {
        if (start == end) {
            return false;
        }
        return start < end
            ? coordinate >= start && coordinate < end
            : coordinate >= start || coordinate < end;
    };
    const auto in_window = [&](uint32_t x, uint32_t y, uint16_t horizontal, uint16_t vertical) {
        return in_window_axis(x, (horizontal >> 8u) & 0xFFu, horizontal & 0xFFu) &&
            in_window_axis(y, (vertical >> 8u) & 0xFFu, vertical & 0xFFu);
    };
    std::vector<uint8_t> window_controls(kVideoPixelCount, 0u);
    std::vector<uint8_t> window_is_explicit(kVideoPixelCount, 0u);
    if (!any_window_enabled) {
        std::fill(window_controls.begin(), window_controls.end(), 0x3Fu);
    } else {
        for (uint32_t y = 0u; y < kVideoHeight; ++y) {
            for (uint32_t x = 0u; x < kVideoWidth; ++x) {
                const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
                if (win0_enabled && in_window(x, y, win0_h, win0_v)) {
                    window_controls[index] = static_cast<uint8_t>(win_in & 0x3Fu);
                    window_is_explicit[index] = 1u;
                } else if (win1_enabled && in_window(x, y, win1_h, win1_v)) {
                    window_controls[index] = static_cast<uint8_t>((win_in >> 8u) & 0x3Fu);
                    window_is_explicit[index] = 1u;
                } else {
                    window_controls[index] = static_cast<uint8_t>(win_out & 0x3Fu);
                }
            }
        }
    }

    const uint16_t mosaic = bus_.Read16(kMosaicAddress);
    const uint32_t obj_mosaic_h = ((mosaic >> 8u) & 0xFu) + 1u;
    const uint32_t obj_mosaic_v = ((mosaic >> 12u) & 0xFu) + 1u;

    auto sample_mode0_text_bg = [&](uint32_t bg, uint32_t screen_x, uint32_t screen_y, uint32_t& out_color) -> bool {
        if (!bg_enabled[bg]) {
            return false;
        }

        const auto& state = video_scanline_states_[std::min(screen_y, kVideoHeight - 1u)];
        const uint16_t cnt = state.bgcnt[bg];
        const bool color_256 = (cnt & (1u << 7u)) != 0u;
        const uint32_t char_base = static_cast<uint32_t>((cnt >> 2u) & 0x3u) * 0x4000u;
        const uint32_t screen_base = static_cast<uint32_t>((cnt >> 8u) & 0x1Fu) * 0x0800u;
        const uint32_t size = (cnt >> 14u) & 0x3u;
        const uint32_t width = (size == 0u || size == 2u) ? 256u : 512u;
        const uint32_t height = (size == 0u || size == 1u) ? 256u : 512u;

        const uint32_t x = (screen_x + state.hofs[bg]) & (width - 1u);
        const uint32_t y = (screen_y + state.vofs[bg]) & (height - 1u);
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
        if (color_256) {
            const uint32_t tile_offset = char_base + (tile_index * 64u) +
                (tile_py * 8u) + tile_px;
            if (tile_offset >= 0x00010000u) {
                return false;
            }
            palette_index = bus_.Read8(kVramBase + tile_offset);
        } else {
            const uint32_t tile_offset = char_base + (tile_index * 32u) +
                (tile_py * 4u) + (tile_px >> 1u);
            if (tile_offset >= 0x00010000u) {
                return false;
            }
            const uint8_t packed = bus_.Read8(kVramBase + tile_offset);
            const uint16_t texel = (tile_px & 1u) == 0u ? (packed & 0x0Fu) : ((packed >> 4u) & 0x0Fu);
            palette_index = static_cast<uint16_t>((palette_bank << 4u) | texel);
            if (texel == 0u) {
                return false;
            }
        }

        if (palette_index == 0u) {
            return false;
        }
        out_color = read_palette_color(palette_index);
        return true;
    };

    auto sample_affine_bg = [&](uint32_t bg, uint32_t screen_x, uint32_t screen_y, uint32_t& out_color) -> bool {
        if (!bg_enabled[bg]) {
            return false;
        }

        const auto& state = video_scanline_states_[std::min(screen_y, kVideoHeight - 1u)];
        const uint16_t cnt = state.bgcnt[bg];
        const uint32_t size = (cnt >> 14u) & 0x3u;
        const uint32_t size_pixels = 128u << size;
        const uint32_t size_fixed = size_pixels << 8u;
        const uint32_t char_base = static_cast<uint32_t>((cnt >> 2u) & 0x3u) * 0x4000u;
        const uint32_t screen_base = static_cast<uint32_t>((cnt >> 8u) & 0x1Fu) * 0x0800u;
        const int32_t pa = state.affine_pa[bg];
        const int32_t pb = state.affine_pb[bg];
        const int32_t pc = state.affine_pc[bg];
        const int32_t pd = state.affine_pd[bg];
        const int32_t reference_x = state.reference_x[bg];
        const int32_t reference_y = state.reference_y[bg];

        int32_t source_x = reference_x + (pa * static_cast<int32_t>(screen_x)) +
            (pb * static_cast<int32_t>(screen_y));
        int32_t source_y = reference_y + (pc * static_cast<int32_t>(screen_x)) +
            (pd * static_cast<int32_t>(screen_y));
        if ((cnt & (1u << 13u)) != 0u) {
            source_x &= static_cast<int32_t>(size_fixed - 1u);
            source_y &= static_cast<int32_t>(size_fixed - 1u);
        } else if (source_x < 0 || source_y < 0 || source_x >= static_cast<int32_t>(size_fixed) ||
                   source_y >= static_cast<int32_t>(size_fixed)) {
            return false;
        }

        const uint32_t pixel_x = static_cast<uint32_t>(source_x) >> 8u;
        const uint32_t pixel_y = static_cast<uint32_t>(source_y) >> 8u;
        const uint32_t tiles_per_row = 16u << size;
        const uint32_t map_address = kVramBase + screen_base + (pixel_y >> 3u) * tiles_per_row + (pixel_x >> 3u);
        const uint32_t tile_address = kVramBase + char_base + (static_cast<uint32_t>(bus_.Read8(map_address)) * 64u) +
            ((pixel_y & 0x7u) * 8u) + (pixel_x & 0x7u);
        const uint8_t palette_index = bus_.Read8(tile_address);
        if (palette_index == 0u) {
            return false;
        }
        out_color = read_palette_color(palette_index);
        return true;
    };

    auto consider_background = [&](size_t index, uint32_t bg, uint32_t color) {
        if ((window_controls[index] & (1u << bg)) == 0u) {
            return;
        }
        const uint8_t priority = static_cast<uint8_t>(bgcnt[bg] & 0x3u);
        const bool in_front = priority < background_priorities[index] ||
            (priority == background_priorities[index] && bg < background_layers[index]);
        if (in_front) {
            second_background_colors[index] = background_colors[index];
            second_background_priorities[index] = background_priorities[index];
            second_background_layers[index] = background_layers[index];
            background_colors[index] = color;
            background_priorities[index] = priority;
            background_layers[index] = static_cast<uint8_t>(bg);
        } else if (priority < second_background_priorities[index] ||
                   (priority == second_background_priorities[index] && bg < second_background_layers[index])) {
            second_background_colors[index] = color;
            second_background_priorities[index] = priority;
            second_background_layers[index] = static_cast<uint8_t>(bg);
        }
    };

    if (mode == 0u || mode == 1u) {
        const uint32_t text_bg_count = mode == 0u ? 4u : 2u;
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
                for (uint32_t bg = 0u; bg < text_bg_count; ++bg) {
                    uint32_t color = 0u;
                    if (sample_mode0_text_bg(bg, x, y, color)) {
                        consider_background(index, bg, color);
                    }
                }
            }
        }
    }
    if (mode == 1u || mode == 2u) {
        const uint32_t first_affine_bg = 2u;
        const uint32_t last_affine_bg = mode == 1u ? 3u : 4u;
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
                for (uint32_t bg = first_affine_bg; bg < last_affine_bg; ++bg) {
                    uint32_t color = 0u;
                    if (sample_affine_bg(bg, x, y, color)) {
                        consider_background(index, bg, color);
                    }
                }
            }
        }
    } else if (mode == 3u && bg2_enabled) {
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
                const uint32_t addr = kVramBase + ((y * kVideoWidth + x) * 2u);
                if ((window_controls[index] & (1u << 2u)) != 0u) {
                    background_colors[index] = Bgr555ToAbgr8888(bus_.Read16(addr));
                    background_priorities[index] = 0u;
                    background_layers[index] = 2u;
                }
            }
        }
    } else if (mode == 4u && bg2_enabled) {
        const uint32_t page_offset = (dispcnt & (1u << 4u)) != 0u ? 0x0000A000u : 0u;
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
                const uint8_t palette_index = bus_.Read8(kVramBase + page_offset + (y * kVideoWidth + x));
                if ((window_controls[index] & (1u << 2u)) != 0u && palette_index != 0u) {
                    background_colors[index] = read_palette_color(palette_index);
                    background_priorities[index] = 0u;
                    background_layers[index] = 2u;
                }
            }
        }
    } else if (mode == 5u && bg2_enabled) {
        const uint32_t page_offset = (dispcnt & (1u << 4u)) != 0u ? 0x0000A000u : 0u;
        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
                if (x < 160u && y < 128u) {
                    const uint32_t addr = kVramBase + page_offset + ((y * 160u + x) * 2u);
                    if ((window_controls[index] & (1u << 2u)) != 0u) {
                        background_colors[index] = Bgr555ToAbgr8888(bus_.Read16(addr));
                        background_priorities[index] = 0u;
                        background_layers[index] = 2u;
                    }
                }
            }
        }
    }

    std::vector<uint32_t> object_colors(kVideoPixelCount, 0u);
    std::vector<uint8_t> object_priorities(kVideoPixelCount, 4u);
    std::vector<uint8_t> object_modes(kVideoPixelCount, 0u);
    std::vector<uint8_t> object_window(kVideoPixelCount, 0u);

    const bool objects_enabled = (dispcnt & (1u << 12u)) != 0u;
    const bool one_dimensional_mapping = (dispcnt & (1u << 6u)) != 0u;
    if (objects_enabled) {
        auto sample_object_pixel = [&](uint16_t attr0, uint16_t attr2, uint32_t source_x, uint32_t source_y,
                                       uint32_t width, uint8_t& out_palette_index) -> bool {
            const bool color_256 = (attr0 & (1u << 13u)) != 0u;
            uint32_t tile = attr2 & 0x03FFu;
            if (color_256 && !one_dimensional_mapping) {
                tile &= ~1u;
            }

            const uint32_t tile_x = source_x / 8u;
            const uint32_t tile_y = source_y / 8u;
            const uint32_t bytes_per_tile = color_256 ? 64u : 32u;
            const uint32_t tiles_per_row = one_dimensional_mapping
                ? (width / 8u)
                : (color_256 ? 16u : 32u);
            uint32_t byte_offset = tile * 32u;
            byte_offset += (tile_y * tiles_per_row + tile_x) * bytes_per_tile;
            byte_offset += (source_y & 7u) * (color_256 ? 8u : 4u);
            byte_offset += color_256 ? (source_x & 7u) : ((source_x & 7u) / 2u);

            const uint8_t packed = bus_.Read8(kObjVramBase + (byte_offset & 0x7FFFu));
            if (color_256) {
                out_palette_index = packed;
                return packed != 0u;
            }
            const uint8_t texel = static_cast<uint8_t>((packed >> ((source_x & 1u) * 4u)) & 0x0Fu);
            if (texel == 0u) {
                return false;
            }
            out_palette_index = static_cast<uint8_t>(((attr2 >> 12u) & 0xFu) * 16u + texel);
            return true;
        };

        for (uint32_t object = 0u; object < 128u; ++object) {
            const uint32_t oam_address = kObjOamBase + object * 8u;
            const uint16_t attr0 = bus_.Read16(oam_address);
            const uint16_t attr1 = bus_.Read16(oam_address + 2u);
            const uint16_t attr2 = bus_.Read16(oam_address + 4u);
            const uint32_t object_mode = (attr0 >> 10u) & 0x3u;
            if (object_mode == 3u || (object_mode == 2u && !objwin_enabled)) {
                continue; // Prohibited modes and disabled OBJ-windows are invisible.
            }
            const bool object_window_sprite = object_mode == 2u;

            const bool affine = (attr0 & (1u << 8u)) != 0u;
            if (!affine && (attr0 & (1u << 9u)) != 0u) {
                continue; // Normal OBJ: bit 9 disables the entry.
            }
            const bool double_size = affine && ((attr0 & (1u << 9u)) != 0u);
            const uint32_t shape = (attr0 >> 14u) & 0x3u;
            const uint32_t size = (attr1 >> 14u) & 0x3u;
            const uint32_t size_index = shape * 4u + size;
            if (size_index >= kObjSizes.size() || kObjSizes[size_index][0] == 0u) {
                continue;
            }

            const uint32_t width = kObjSizes[size_index][0];
            const uint32_t height = kObjSizes[size_index][1];
            if (mode >= 3u && (attr2 & 0x03FFu) < 512u) {
                continue; // The lower OBJ tile region is reserved in bitmap modes.
            }
            const uint8_t object_priority = static_cast<uint8_t>((attr2 >> 10u) & 0x3u);
            const int object_x = (attr1 & 0x01FFu) >= 256u
                ? static_cast<int>(attr1 & 0x01FFu) - 512
                : static_cast<int>(attr1 & 0x01FFu);
            const int object_y = static_cast<int>(attr0 & 0x00FFu);
            if (!affine) {
                const bool hflip = (attr1 & (1u << 12u)) != 0u;
                const bool vflip = (attr1 & (1u << 13u)) != 0u;
                const bool mosaic_enabled = (attr0 & (1u << 12u)) != 0u;
                for (uint32_t screen_y = 0u; screen_y < kVideoHeight; ++screen_y) {
                    uint32_t sample_screen_y = screen_y;
                    if (mosaic_enabled && obj_mosaic_v > 1u) {
                        sample_screen_y -= sample_screen_y % obj_mosaic_v;
                        if (sample_screen_y < static_cast<uint32_t>(object_y) && object_y < static_cast<int>(kVideoHeight)) {
                            sample_screen_y = static_cast<uint32_t>(object_y);
                        }
                    }
                    int source_y = static_cast<int>(sample_screen_y) - object_y;
                    if (source_y < 0) {
                        source_y += 256;
                    }
                    if (source_y < 0 || source_y >= static_cast<int>(height)) {
                        continue;
                    }
                    if (vflip) {
                        source_y = static_cast<int>(height) - source_y - 1;
                    }
                    const int screen_x_start = std::max(object_x, 0);
                    const int screen_x_end = std::min(object_x + static_cast<int>(width), static_cast<int>(kVideoWidth));
                    for (int screen_x_value = screen_x_start; screen_x_value < screen_x_end; ++screen_x_value) {
                        const uint32_t screen_x = static_cast<uint32_t>(screen_x_value);
                        uint32_t sample_screen_x = screen_x;
                        if (mosaic_enabled && obj_mosaic_h > 1u) {
                            sample_screen_x -= sample_screen_x % obj_mosaic_h;
                            if (sample_screen_x < static_cast<uint32_t>(std::max(object_x, 0)) && object_x < static_cast<int>(kVideoWidth)) {
                                sample_screen_x = static_cast<uint32_t>(std::max(object_x, 0));
                            }
                        }
                        const int local_x = static_cast<int>(sample_screen_x) - object_x;
                        if (local_x < 0 || local_x >= static_cast<int>(width)) {
                            continue;
                        }
                        const uint32_t source_x = hflip
                            ? width - static_cast<uint32_t>(local_x) - 1u
                            : static_cast<uint32_t>(local_x);
                        uint8_t palette_index = 0u;
                        if (!sample_object_pixel(attr0, attr2, source_x, static_cast<uint32_t>(source_y), width, palette_index)) {
                            continue;
                        }
                        const size_t index = static_cast<size_t>(screen_y) * kVideoWidth + screen_x;
                        if (object_window_sprite) {
                            object_window[index] = 1u;
                        } else if (object_priorities[index] > object_priority) {
                            const uint16_t absolute_palette = static_cast<uint16_t>(0x100u + palette_index);
                            object_colors[index] = read_palette_color(absolute_palette);
                            object_priorities[index] = object_priority;
                            object_modes[index] = static_cast<uint8_t>(object_mode);
                        }
                    }
                }
                continue;
            }

            const uint32_t matrix_index = (attr1 >> 9u) & 0x1Fu;
            const uint32_t matrix_address = kObjOamBase + matrix_index * 32u + 6u;
            const int32_t pa = static_cast<int16_t>(bus_.Read16(matrix_address));
            const int32_t pb = static_cast<int16_t>(bus_.Read16(matrix_address + 8u));
            const int32_t pc = static_cast<int16_t>(bus_.Read16(matrix_address + 16u));
            const int32_t pd = static_cast<int16_t>(bus_.Read16(matrix_address + 24u));
            const uint32_t draw_width = width << (double_size ? 1u : 0u);
            const uint32_t draw_height = height << (double_size ? 1u : 0u);

            const bool mosaic_enabled = (attr0 & (1u << 12u)) != 0u;
            for (uint32_t screen_y = 0u; screen_y < kVideoHeight; ++screen_y) {
                uint32_t sample_screen_y = screen_y;
                if (mosaic_enabled && obj_mosaic_v > 1u) {
                    sample_screen_y -= sample_screen_y % obj_mosaic_v;
                    if (sample_screen_y < static_cast<uint32_t>(object_y) && object_y < static_cast<int>(kVideoHeight)) {
                        sample_screen_y = static_cast<uint32_t>(object_y);
                    }
                }
                int local_y = static_cast<int>(sample_screen_y) - object_y;
                if (local_y < 0) {
                    local_y += 256;
                }
                if (local_y < 0 || local_y >= static_cast<int>(draw_height)) {
                    continue;
                }
                const int screen_x_start = std::max(object_x, 0);
                const int screen_x_end = std::min(object_x + static_cast<int>(draw_width), static_cast<int>(kVideoWidth));
                for (int screen_x_value = screen_x_start; screen_x_value < screen_x_end; ++screen_x_value) {
                    const uint32_t screen_x = static_cast<uint32_t>(screen_x_value);
                    uint32_t sample_screen_x = screen_x;
                    if (mosaic_enabled && obj_mosaic_h > 1u) {
                        sample_screen_x -= sample_screen_x % obj_mosaic_h;
                        if (sample_screen_x < static_cast<uint32_t>(std::max(object_x, 0)) && object_x < static_cast<int>(kVideoWidth)) {
                            sample_screen_x = static_cast<uint32_t>(std::max(object_x, 0));
                        }
                    }
                    const int local_x = static_cast<int>(sample_screen_x) - object_x;
                    if (local_x < 0 || local_x >= static_cast<int>(draw_width)) {
                        continue;
                    }
                    const int32_t centered_x = local_x - static_cast<int>(draw_width / 2u);
                    const int32_t centered_y = local_y - static_cast<int>(draw_height / 2u);
                    const int32_t source_x_fixed = pa * centered_x + pb * centered_y + static_cast<int32_t>(width << 7u);
                    const int32_t source_y_fixed = pc * centered_x + pd * centered_y + static_cast<int32_t>(height << 7u);
                    const int32_t source_x = source_x_fixed >> 8u;
                    const int32_t source_y = source_y_fixed >> 8u;
                    if (source_x < 0 || source_y < 0 || source_x >= static_cast<int32_t>(width) || source_y >= static_cast<int32_t>(height)) {
                        continue;
                    }
                    uint8_t palette_index = 0u;
                    if (!sample_object_pixel(attr0, attr2, static_cast<uint32_t>(source_x), static_cast<uint32_t>(source_y), width, palette_index)) {
                        continue;
                    }
                    const size_t index = static_cast<size_t>(screen_y) * kVideoWidth + screen_x;
                    if (object_window_sprite) {
                        object_window[index] = 1u;
                    } else if (object_priorities[index] > object_priority) {
                        const uint16_t absolute_palette = static_cast<uint16_t>(0x100u + palette_index);
                        object_colors[index] = read_palette_color(absolute_palette);
                        object_priorities[index] = object_priority;
                        object_modes[index] = static_cast<uint8_t>(object_mode);
                    }
                }
            }
        }
    }

    if (objwin_enabled) {
        for (size_t index = 0u; index < kVideoPixelCount; ++index) {
            if (object_window[index] != 0u && window_is_explicit[index] == 0u) {
                window_controls[index] = static_cast<uint8_t>((win_out >> 8u) & 0x3Fu);
            }
        }
    }

    const uint16_t blend_control = bus_.Read16(kBlendCntAddress);
    const uint16_t blend_alpha = bus_.Read16(kBlendAlphaAddress);
    const uint16_t blend_y = bus_.Read16(kBlendYAddress);
    const uint32_t blend_effect = (blend_control >> 6u) & 0x3u;
    const uint32_t eva = std::min<uint32_t>(blend_alpha & 0x1Fu, 16u);
    const uint32_t evb = std::min<uint32_t>((blend_alpha >> 8u) & 0x1Fu, 16u);
    const uint32_t bldy = std::min<uint32_t>(blend_y & 0x1Fu, 16u);

    auto target1_enabled = [&](uint8_t layer) {
        return (blend_control & (1u << (layer == 4u ? 5u : layer))) != 0u;
    };
    auto target2_enabled = [&](uint8_t layer) {
        return (blend_control & (1u << (8u + (layer == 4u ? 5u : layer)))) != 0u;
    };

    for (uint32_t y = 0u; y < kVideoHeight; ++y) {
        for (uint32_t x = 0u; x < kVideoWidth; ++x) {
            const size_t index = static_cast<size_t>(y) * kVideoWidth + x;
            uint32_t color = background_colors[index];
            const bool object_visible = object_priorities[index] < 4u &&
                object_priorities[index] <= background_priorities[index] &&
                (window_controls[index] & (1u << 4u)) != 0u;
            const bool blend_enabled = (window_controls[index] & (1u << 5u)) != 0u;
            if (object_visible) {
                if (object_modes[index] == 1u && blend_effect == 1u &&
                    blend_enabled && target2_enabled(background_layers[index])) {
                    color = MixColors(object_colors[index], color, eva, evb);
                } else {
                    color = object_colors[index];
                }
                if (blend_effect == 2u && (blend_control & (1u << 4u)) != 0u && blend_enabled) {
                    color = BrightenColor(color, bldy);
                } else if (blend_effect == 3u && (blend_control & (1u << 4u)) != 0u && blend_enabled) {
                    color = DarkenColor(color, bldy);
                }
            } else if (blend_effect == 1u && blend_enabled && target1_enabled(background_layers[index]) &&
                       target2_enabled(second_background_layers[index])) {
                color = MixColors(color, second_background_colors[index], eva, evb);
            } else if (blend_effect == 2u && blend_enabled && target1_enabled(background_layers[index])) {
                color = BrightenColor(color, bldy);
            } else if (blend_effect == 3u && blend_enabled && target1_enabled(background_layers[index])) {
                color = DarkenColor(color, bldy);
            }
            video_buffer_[index] = color;
        }
    }

    const char* dump_frame = std::getenv("EMUGBDS_DUMP_FRAME");
    if (dump_frame != nullptr && std::strtoull(dump_frame, nullptr, 10) == frame_counter_) {
        FILE* output = std::fopen("build/debug-frame.ppm", "wb");
        if (output != nullptr) {
            std::fprintf(output, "P6\n%u %u\n255\n", kVideoWidth, kVideoHeight);
            for (uint32_t color : video_buffer_) {
                const uint8_t rgb[3] = {
                    static_cast<uint8_t>(color & 0xFFu),
                    static_cast<uint8_t>((color >> 8u) & 0xFFu),
                    static_cast<uint8_t>((color >> 16u) & 0xFFu),
                };
                std::fwrite(rgb, sizeof(rgb), 1u, output);
            }
            std::fclose(output);
        }
    }
}


} // namespace emugbds::ingba
