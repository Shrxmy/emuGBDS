#pragma once

// One DS 2D graphics engine (A at 4000000h, B at 4001000h): its registers and
// a scanline renderer for text, affine, extended affine and bitmap BGs, OBJs,
// windows, blending, and master brightness (GBATEK "DS Video").
// Engine A also has the display capture unit (DISPCAPCNT, GBATEK "DS Video Capture").
// Engine A shows the 3D engine's picture as BG0 when DISPCNT selects it.
// shortcut: no mosaic, and main-memory display mode (and its capture source) shows black.

#include "emugbds/inds/Gpu3d.h"
#include "emugbds/inds/VideoMemory.h"

#include <array>
#include <cstdint>

namespace emugbds::inds {

// 5-bit colour channel -> channel, for brightness effects.
using ChannelTable = std::array<uint8_t, 32>;

class Engine2d {
public:
    static constexpr uint32_t kWidth = 256u;
    static constexpr uint32_t kHeight = 192u;
    static constexpr uint32_t kRegisterBytes = 0x70u;

    // `gpu3d` is the 3D engine whose picture engine A shows as BG0 (nullptr for engine B).
    Engine2d(VideoMemory& memory, int engine, const Gpu3d* gpu3d) noexcept : memory_(memory), engine_(engine), gpu3d_(gpu3d) {}
    uint16_t disp3dcnt() const noexcept { return Reg16(0x60u); }
    void Reset() noexcept;
    // offset = address - engine base (word aligned); mask selects byte lanes.
    uint32_t ReadWord(uint32_t offset) const noexcept;
    void WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept;
    // Renders line y (0-191) into ABGR8888 pixels; `enabled` is the engine's POWCNT1 bit.
    void RenderLine(uint32_t y, bool enabled, uint32_t* out) noexcept;
    // Debug: layers to leave out (bits 0-3 BG0-BG3, bit 4 OBJ); kept across Reset.
    void set_hidden_layers(uint8_t mask) noexcept { hidden_layers_ = mask; }

private:
    struct ObjPixel {
        int32_t color = -1; // 15-bit colour, or -1 when transparent
        uint8_t priority = 4;
        bool blend = false; // semi-transparent or bitmap OBJ
        uint8_t alpha = 0;  // bitmap OBJ EVA (1-16); 0 = semi-transparent OBJ using BLDALPHA
    };

    uint16_t Reg16(uint32_t offset) const noexcept {
        return static_cast<uint16_t>(regs_[offset] | (regs_[offset + 1u] << 8u));
    }
    uint32_t Reg32(uint32_t offset) const noexcept {
        return Reg16(offset) | (static_cast<uint32_t>(Reg16(offset + 2u)) << 16u);
    }
    uint32_t dispcnt() const noexcept { return Reg32(0x00u); }

    void DrawLine(uint32_t y, bool enabled, uint32_t* out) noexcept;
    // Layers and effects of line y into composed_ (15-bit colours, before master brightness).
    void ComposeLine(uint32_t y, uint32_t control) noexcept;
    // Display capture of line y (engine A); `composed` says whether ComposeLine drew this line.
    void CaptureLine(uint32_t y, bool composed) noexcept;
    void RenderTextBg(uint32_t bg, uint32_t y, std::array<int32_t, kWidth>& line) const noexcept;
    void RenderAffineBg(uint32_t bg, uint32_t kind, std::array<int32_t, kWidth>& line) const noexcept;
    void RenderObjs(uint32_t y) noexcept;
    // Compositing settings that are fixed for a whole line, read once instead of per pixel.
    struct LineSetup {
        uint16_t backdrop = 0;
        uint16_t bldcnt = 0;
        uint32_t eva = 0;
        uint32_t evb = 0;
        uint32_t evy = 0;
        bool windows = false;
        std::array<uint8_t, kWidth> window{}; // per-pixel layer masks, when windows are on
        ChannelTable effect{};                 // BLDCNT brightness up/down by EVY
        uint32_t bg_count = 0;
        std::array<uint8_t, 4> bgs{};         // active BGs, by priority then number
        std::array<uint8_t, 4> bg_priority{};
    };

    LineSetup SetupLine(uint32_t y) const noexcept;
    // Each pixel's visible layers (bits 0-3 BGs, 4 OBJ, 5 effects) on line y.
    void FillWindowMasks(uint32_t y, std::array<uint8_t, kWidth>& masks) const noexcept;
    uint16_t Compose(uint32_t x, const LineSetup& setup) const noexcept;
    // Compose for lines without windows or colour effects: the top layer's colour.
    uint16_t TopColor(uint32_t x, const LineSetup& setup) const noexcept;

    VideoMemory& memory_;
    int engine_;
    const Gpu3d* gpu3d_;
    uint8_t hidden_layers_ = 0;
    std::array<uint8_t, kRegisterBytes> regs_{};
    std::array<int32_t, 2> ref_x_{}; // BG2/BG3 internal affine reference points
    std::array<int32_t, 2> ref_y_{};
    std::array<std::array<int32_t, kWidth>, 4> bg_line_{};
    std::array<bool, 4> bg_active_{};
    std::array<ObjPixel, kWidth> obj_line_{};
    std::array<bool, kWidth> obj_window_{};
    bool obj_blend_ = false; // a semi-transparent or bitmap OBJ was drawn on this line
    std::array<uint8_t, kWidth> bg0_alpha_{}; // 3D BG0: per-pixel alpha (31 = opaque)
    bool bg0_translucent_ = false;            // a 3D pixel on this line is translucent
    std::array<uint16_t, kWidth> composed_{};
    bool capturing_ = false; // a capture started at line 0 of this frame
};

} // namespace emugbds::inds
