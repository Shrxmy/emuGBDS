#include "emugbds/inds/Gpu3d.h"

#include <algorithm>
#include <cmath>

// Rendering engine: draws the polygon list handed over by the last SWAP_BUFFERS into the frame
// buffer (GBATEK "DS 3D Texture Blending", "Toon, Edge, Fog, Alpha-Blending", "Rear-Plane").

namespace emugbds::inds {
namespace {

using Area = VideoMemory::Area;

constexpr uint32_t kAlphaTestRef = 0x10u; // render register offsets from 4000330h
constexpr uint32_t kClearColor = 0x20u;
constexpr uint32_t kClearDepth = 0x24u;
constexpr uint32_t kClearImageOffset = 0x26u;
constexpr uint32_t kToonTable = 0x50u;

int32_t Expand5(uint32_t channel) noexcept {
    return channel == 0u ? 0 : static_cast<int32_t>(channel * 2u + 1u);
}

// The 15-bit rear-plane depth as 24 bits.
uint32_t Depth15(uint32_t depth) noexcept {
    return depth * 0x200u + ((depth + 1u) / 0x8000u) * 0x1FFu;
}

int32_t Wrap(int32_t coordinate, int32_t size, bool repeat, bool flip) noexcept {
    if (!repeat) {
        return std::clamp(coordinate, 0, size - 1);
    }
    if (!flip) {
        return coordinate & (size - 1);
    }
    const int32_t period = coordinate & (2 * size - 1);
    return period < size ? period : 2 * size - 1 - period;
}

// A vertex on screen, with its attributes divided by w for perspective-correct interpolation.
struct ScreenVertex {
    double x = 0.0, y = 0.0;
    double inv_w = 0.0;
    double depth = 0.0; // 24-bit Z-buffer depth (linear on screen)
    double r = 0.0, g = 0.0, b = 0.0, s = 0.0, t = 0.0;
};

ScreenVertex Lerp(const ScreenVertex& a, const ScreenVertex& b, double t) noexcept {
    ScreenVertex v;
    v.x = a.x + (b.x - a.x) * t;
    v.y = a.y + (b.y - a.y) * t;
    v.inv_w = a.inv_w + (b.inv_w - a.inv_w) * t;
    v.depth = a.depth + (b.depth - a.depth) * t;
    v.r = a.r + (b.r - a.r) * t;
    v.g = a.g + (b.g - a.g) * t;
    v.b = a.b + (b.b - a.b) * t;
    v.s = a.s + (b.s - a.s) * t;
    v.t = a.t + (b.t - a.t) * t;
    return v;
}

} // namespace

void Gpu3d::Render(uint16_t disp3dcnt) noexcept {
    ClearFrame(disp3dcnt);
    const bool w_buffer = (rendering_flags_ & 2u) != 0u;
    std::vector<const Polygon*> translucent;
    for (const Polygon& polygon : rendering_) {
        if (polygon.translucent) {
            translucent.push_back(&polygon);
        } else {
            DrawPolygon(polygon, disp3dcnt, w_buffer);
        }
    }
    if ((rendering_flags_ & 1u) == 0u) { // auto-sort translucent polygons by their screen position
        const auto bottom = [](const Polygon* polygon) {
            double lowest = -1e30;
            for (uint32_t i = 0u; i < polygon->count; ++i) {
                const Vertex& v = polygon->vertices[i];
                lowest = std::max(lowest, -static_cast<double>(v.y) / std::max(v.w, 1));
            }
            return lowest;
        };
        std::stable_sort(translucent.begin(), translucent.end(),
            [&](const Polygon* a, const Polygon* b) { return bottom(a) < bottom(b); });
    }
    for (const Polygon* polygon : translucent) {
        DrawPolygon(*polygon, disp3dcnt, w_buffer);
    }
    for (uint32_t i = 0u; i < kWidth * kHeight; ++i) {
        const uint32_t rgb = rgb6_[i];
        color_[i] = static_cast<uint16_t>(((rgb & 0x3Fu) >> 1u) | ((((rgb >> 8u) & 0x3Fu) >> 1u) << 5u) |
            ((((rgb >> 16u) & 0x3Fu) >> 1u) << 10u));
        alpha_[i] = alpha5_[i];
    }
}


void Gpu3d::ClearFrame(uint16_t disp3dcnt) noexcept {
    const auto reg16 = [this](uint32_t offset) {
        return static_cast<uint32_t>(render_regs_[offset] | (render_regs_[offset + 1u] << 8u));
    };
    const auto pack = [](uint32_t color) {
        return static_cast<uint32_t>(Expand5(color & 31u)) | (static_cast<uint32_t>(Expand5((color >> 5u) & 31u)) << 8u) |
            (static_cast<uint32_t>(Expand5((color >> 10u) & 31u)) << 16u);
    };
    std::fill(translucent_id_.begin(), translucent_id_.end(), static_cast<int8_t>(-1));
    if ((disp3dcnt & (1u << 14)) != 0u) {
        // Rear-plane bitmaps: colour in texture slot 2, depth in slot 3, both 256x256 and scrollable.
        const uint32_t offset = reg16(kClearImageOffset);
        for (uint32_t y = 0u; y < kHeight; ++y) {
            for (uint32_t x = 0u; x < kWidth; ++x) {
                const uint32_t texel = ((((y + (offset >> 8u)) & 0xFFu) * 256u) + ((x + offset) & 0xFFu)) * 2u;
                const uint16_t color = video_.Read16(Area::Texture, 0x40000u + texel);
                const uint16_t depth = video_.Read16(Area::Texture, 0x60000u + texel);
                const uint32_t pixel = y * kWidth + x;
                rgb6_[pixel] = pack(color);
                alpha5_[pixel] = (color & 0x8000u) != 0u ? 31u : 0u;
                depth_[pixel] = Depth15(depth & 0x7FFFu);
            }
        }
        return;
    }
    const uint32_t clear = reg16(kClearColor) | (reg16(kClearColor + 2u) << 16u);
    std::fill(rgb6_.begin(), rgb6_.end(), pack(clear));
    std::fill(alpha5_.begin(), alpha5_.end(), static_cast<uint8_t>((clear >> 16u) & 31u));
    std::fill(depth_.begin(), depth_.end(), Depth15(reg16(kClearDepth) & 0x7FFFu));
}


void Gpu3d::TexelColor(const Polygon& polygon, int32_t s, int32_t t, int32_t& r, int32_t& g, int32_t& b, int32_t& a) const noexcept {
    const uint32_t param = polygon.texture;
    const uint32_t format = (param >> 26u) & 7u;
    const int32_t width = 8 << ((param >> 20u) & 7u);
    const int32_t height = 8 << ((param >> 23u) & 7u);
    const int32_t u = Wrap(s >> 4, width, (param & (1u << 16)) != 0u, (param & (1u << 18)) != 0u);
    const int32_t v = Wrap(t >> 4, height, (param & (1u << 17)) != 0u, (param & (1u << 19)) != 0u);
    const uint32_t base = (param & 0xFFFFu) * 8u;
    const uint32_t texel = static_cast<uint32_t>(v * width + u);
    const bool transparent0 = (param & (1u << 29)) != 0u;
    const uint32_t palette = polygon.palette * (format == 2u ? 8u : 16u);
    const auto image8 = [this](uint32_t address) { return video_.Read8(Area::Texture, address & 0x7FFFFu); };
    const auto image16 = [this](uint32_t address) { return video_.Read16(Area::Texture, address & 0x7FFFFu); };
    const auto palette16 = [this](uint32_t address) { return video_.Read16(Area::TexturePalette, address % 0x18000u); };
    const auto set = [&](uint16_t color, int32_t alpha) {
        r = Expand5(color & 31u);
        g = Expand5((color >> 5u) & 31u);
        b = Expand5((color >> 10u) & 31u);
        a = alpha;
    };
    switch (format) {
    case 1u: { // A3I5
        const uint8_t byte = image8(base + texel);
        const int32_t alpha3 = byte >> 5u;
        set(palette16(palette + (byte & 31u) * 2u), alpha3 * 4 + alpha3 / 2);
        return;
    }
    case 2u: { // 4 colours
        const uint32_t index = (image8(base + texel / 4u) >> ((texel & 3u) * 2u)) & 3u;
        set(palette16(palette + index * 2u), transparent0 && index == 0u ? 0 : 31);
        return;
    }
    case 3u: { // 16 colours
        const uint32_t index = (image8(base + texel / 2u) >> ((texel & 1u) * 4u)) & 15u;
        set(palette16(palette + index * 2u), transparent0 && index == 0u ? 0 : 31);
        return;
    }
    case 4u: { // 256 colours
        const uint32_t index = image8(base + texel);
        set(palette16(palette + index * 2u), transparent0 && index == 0u ? 0 : 31);
        return;
    }
    case 5u: { // 4x4-texel compressed: 2 bits per texel, palette info per block in slot 1
        const uint32_t block = base + static_cast<uint32_t>((v / 4) * (width / 4) + u / 4) * 4u;
        const uint32_t bits = image16(block) | (static_cast<uint32_t>(image16(block + 2u)) << 16u);
        const uint32_t index = (bits >> ((v & 3) * 8 + (u & 3) * 2)) & 3u;
        const uint32_t info_address = 0x20000u + ((block & 0x1FFFFu) >> 1u) + ((block & 0x7FFFFu) >= 0x40000u ? 0x10000u : 0u);
        const uint16_t info = image16(info_address);
        const uint32_t colors = palette + (info & 0x3FFFu) * 4u;
        const uint32_t mode = info >> 14u;
        const uint16_t c0 = palette16(colors);
        const uint16_t c1 = palette16(colors + 2u);
        const auto mix = [&](uint32_t w0, uint32_t w1, uint32_t divisor) {
            uint16_t color = 0u;
            for (uint32_t shift = 0u; shift < 15u; shift += 5u) {
                color = static_cast<uint16_t>(color | ((((c0 >> shift) & 31u) * w0 + ((c1 >> shift) & 31u) * w1) / divisor) << shift);
            }
            return color;
        };
        if (index == 0u) {
            set(c0, 31);
        } else if (index == 1u) {
            set(c1, 31);
        } else if (mode == 0u || mode == 2u) {
            if (index == 3u && mode == 0u) {
                set(0u, 0);
            } else {
                set(palette16(colors + index * 2u), 31);
            }
        } else if (mode == 1u) {
            if (index == 2u) {
                set(mix(1u, 1u, 2u), 31);
            } else {
                set(0u, 0);
            }
        } else {
            set(index == 2u ? mix(5u, 3u, 8u) : mix(3u, 5u, 8u), 31);
        }
        return;
    }
    case 6u: { // A5I3
        const uint8_t byte = image8(base + texel);
        set(palette16(palette + (byte & 7u) * 2u), byte >> 3u);
        return;
    }
    case 7u: { // direct colour
        const uint16_t color = image16(base + texel * 2u);
        set(color, (color & 0x8000u) != 0u ? 31 : 0);
        return;
    }
    default:
        set(0x7FFFu, 31);
        return;
    }
}


void Gpu3d::DrawPolygon(const Polygon& polygon, uint16_t disp3dcnt, bool w_buffer) noexcept {
    const double x1 = polygon.viewport[0];
    const double y1 = polygon.viewport[1];
    const double width = static_cast<double>(polygon.viewport[2]) - polygon.viewport[0] + 1.0;
    const double height = static_cast<double>(polygon.viewport[3]) - polygon.viewport[1] + 1.0;
    std::array<ScreenVertex, 10> screen{};
    double top = 1e30;
    double bottom = -1e30;
    for (uint32_t i = 0u; i < polygon.count; ++i) {
        const Vertex& v = polygon.vertices[i];
        const double w = v.w > 0 ? v.w : 1.0;
        ScreenVertex& p = screen[i];
        p.x = (v.x + w) * width / (2.0 * w) + x1;
        p.y = static_cast<double>(kHeight) - ((v.y + w) * height / (2.0 * w) + y1); // viewport y counts from the bottom
        p.inv_w = 1.0 / w;
        p.depth = std::clamp((static_cast<double>(v.z) / w * 0x4000 + 0x3FFF) * 0x200, 0.0, 16777215.0);
        p.r = v.r * p.inv_w;
        p.g = v.g * p.inv_w;
        p.b = v.b * p.inv_w;
        p.s = v.s * p.inv_w;
        p.t = v.t * p.inv_w;
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }

    const uint32_t attr = polygon.attr;
    const uint32_t mode = (attr >> 4u) & 3u;
    const int32_t polygon_alpha = static_cast<int32_t>((attr >> 16u) & 31u);
    const bool wireframe = polygon_alpha == 0;
    const int32_t vertex_alpha = wireframe ? 31 : polygon_alpha;
    const int8_t id = static_cast<int8_t>((attr >> 24u) & 63u);
    const bool textured = (disp3dcnt & 1u) != 0u && ((polygon.texture >> 26u) & 7u) != 0u;
    const bool highlight = (disp3dcnt & 2u) != 0u;
    const int32_t alpha_ref = (disp3dcnt & 4u) != 0u ? render_regs_[kAlphaTestRef] & 31 : 0;
    const bool blending = (disp3dcnt & 8u) != 0u;
    const bool depth_equal = (attr & (1u << 14)) != 0u;
    const bool update_translucent_depth = (attr & (1u << 11)) != 0u;

    int32_t first_row = static_cast<int32_t>(std::ceil(top - 0.5));
    int32_t last_row = static_cast<int32_t>(std::ceil(bottom - 0.5)) - 1;
    if (last_row < first_row) { // flat polygon or horizontal line: one row
        first_row = last_row = static_cast<int32_t>(std::floor((top + bottom) / 2.0));
    }
    for (int32_t row = std::max(first_row, 0); row <= std::min(last_row, static_cast<int32_t>(kHeight) - 1); ++row) {
        const double center = std::clamp(row + 0.5, top, bottom);
        // Where the polygon's edges cross this row: the left-most and right-most crossings.
        ScreenVertex left;
        ScreenVertex right;
        bool found = false;
        const auto take = [&](const ScreenVertex& point) {
            if (!found || point.x < left.x) {
                left = point;
            }
            if (!found || point.x > right.x) {
                right = point;
            }
            found = true;
        };
        for (uint32_t i = 0u; i < polygon.count; ++i) {
            const ScreenVertex& a = screen[i];
            const ScreenVertex& b = screen[(i + 1u) % polygon.count];
            if (a.y == b.y) {
                if (a.y == center) { // an edge lying on the row: both ends
                    take(a);
                    take(b);
                }
            } else if ((a.y <= center && center <= b.y) || (b.y <= center && center <= a.y)) {
                take(Lerp(a, b, (center - a.y) / (b.y - a.y)));
            }
        }
        if (!found) {
            continue;
        }
        int32_t first_column = static_cast<int32_t>(std::ceil(left.x - 0.5));
        int32_t last_column = static_cast<int32_t>(std::ceil(right.x - 0.5)) - 1;
        if (last_column < first_column) {
            last_column = first_column; // lines and dots still cover a pixel
        }
        const double span = right.x - left.x;
        for (int32_t column = std::max(first_column, 0); column <= std::min(last_column, static_cast<int32_t>(kWidth) - 1); ++column) {
            if (wireframe && column != first_column && column != last_column && row != first_row && row != last_row) {
                continue; // wire-frame polygons draw only their outline
            }
            const double t = span > 1e-9 ? std::clamp((column + 0.5 - left.x) / span, 0.0, 1.0) : 0.0;
            const ScreenVertex p = Lerp(left, right, t);
            const double w = 1.0 / p.inv_w;
            const uint32_t depth = w_buffer ? static_cast<uint32_t>(std::clamp(w, 0.0, 16777215.0)) : static_cast<uint32_t>(p.depth);
            const uint32_t pixel = static_cast<uint32_t>(row) * kWidth + static_cast<uint32_t>(column);
            const bool depth_ok = depth_equal ? (depth > depth_[pixel] ? depth - depth_[pixel] : depth_[pixel] - depth) <= 0x200u
                                              : depth < depth_[pixel];
            if (!depth_ok) {
                continue;
            }

            int32_t red = std::clamp(static_cast<int32_t>(p.r * w + 0.5), 0, 63);
            int32_t green = std::clamp(static_cast<int32_t>(p.g * w + 0.5), 0, 63);
            int32_t blue = std::clamp(static_cast<int32_t>(p.b * w + 0.5), 0, 63);
            int32_t alpha = vertex_alpha;
            if (mode == 2u) { // toon/highlight: red indexes the toon table
                const uint32_t entry = kToonTable + static_cast<uint32_t>(red >> 1) * 2u;
                const uint32_t toon = render_regs_[entry] | (render_regs_[entry + 1u] << 8u);
                const int32_t toon_r = Expand5(toon & 31u);
                const int32_t toon_g = Expand5((toon >> 5u) & 31u);
                const int32_t toon_b = Expand5((toon >> 10u) & 31u);
                red = toon_r;
                green = toon_g;
                blue = toon_b;
            }
            if (textured) {
                int32_t tr = 0, tg = 0, tb = 0, ta = 0;
                TexelColor(polygon, static_cast<int32_t>(std::floor(p.s * w)), static_cast<int32_t>(std::floor(p.t * w)), tr, tg, tb, ta);
                if (mode == 1u) { // decal
                    if (ta == 31) {
                        red = tr;
                        green = tg;
                        blue = tb;
                    } else if (ta != 0) {
                        red = (tr * ta + red * (31 - ta)) / 31;
                        green = (tg * ta + green * (31 - ta)) / 31;
                        blue = (tb * ta + blue * (31 - ta)) / 31;
                    }
                } else { // modulation, and toon/highlight with the table colour
                    red = ((tr + 1) * (red + 1) - 1) / 64;
                    green = ((tg + 1) * (green + 1) - 1) / 64;
                    blue = ((tb + 1) * (blue + 1) - 1) / 64;
                    alpha = ((ta + 1) * (alpha + 1) - 1) / 32;
                }
            }
            if (mode == 2u && highlight) { // highlight shading adds the table colour once more
                const uint32_t entry = kToonTable + static_cast<uint32_t>(std::clamp(static_cast<int32_t>(p.r * w + 0.5), 0, 63) >> 1) * 2u;
                const uint32_t toon = render_regs_[entry] | (render_regs_[entry + 1u] << 8u);
                red = std::min(red + Expand5(toon & 31u), 63);
                green = std::min(green + Expand5((toon >> 5u) & 31u), 63);
                blue = std::min(blue + Expand5((toon >> 10u) & 31u), 63);
            }
            if (alpha <= alpha_ref || alpha == 0) {
                continue; // alpha test (alpha 0 is never drawn)
            }
            const uint32_t rgb = static_cast<uint32_t>(red) | (static_cast<uint32_t>(green) << 8u) | (static_cast<uint32_t>(blue) << 16u);
            if (alpha < 31) {
                if (translucent_id_[pixel] == id) {
                    continue; // a translucent polygon does not blend twice over its own ID
                }
                const uint32_t old_alpha = alpha5_[pixel];
                if (blending && old_alpha != 0u) {
                    const uint32_t old = rgb6_[pixel];
                    uint32_t mixed = 0u;
                    for (uint32_t shift = 0u; shift < 24u; shift += 8u) {
                        const uint32_t channel = (((rgb >> shift) & 0x3Fu) * static_cast<uint32_t>(alpha + 1) +
                                                     ((old >> shift) & 0x3Fu) * static_cast<uint32_t>(31 - alpha)) / 32u;
                        mixed |= channel << shift;
                    }
                    rgb6_[pixel] = mixed;
                    alpha5_[pixel] = static_cast<uint8_t>(std::max<uint32_t>(static_cast<uint32_t>(alpha), old_alpha));
                } else {
                    rgb6_[pixel] = rgb;
                    alpha5_[pixel] = static_cast<uint8_t>(alpha);
                }
                if (update_translucent_depth) {
                    depth_[pixel] = depth;
                }
                translucent_id_[pixel] = id;
            } else {
                rgb6_[pixel] = rgb;
                alpha5_[pixel] = 31u;
                depth_[pixel] = depth;
                translucent_id_[pixel] = -1;
            }
        }
    }
}

} // namespace emugbds::inds
