#pragma once

// One DS 2D graphics engine (A at 4000000h, B at 4001000h): its registers and
// a scanline renderer for text, affine, extended affine and bitmap BGs, OBJs,
// windows, blending, and master brightness (GBATEK "DS Video").
// shortcut: no mosaic, no 3D (BG0 in 3D mode is transparent), no display
// capture, and main-memory display mode shows black.

#include "emugbds/inds/VideoMemory.h"

#include <array>
#include <cstdint>

namespace emugbds::inds {

class Engine2d {
public:
    static constexpr uint32_t kWidth = 256u;
    static constexpr uint32_t kRegisterBytes = 0x70u;

    Engine2d(VideoMemory& memory, int engine) noexcept : memory_(memory), engine_(engine) {}
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
    void RenderTextBg(uint32_t bg, uint32_t y, std::array<int32_t, kWidth>& line) const noexcept;
    void RenderAffineBg(uint32_t bg, uint32_t kind, std::array<int32_t, kWidth>& line) const noexcept;
    void RenderObjs(uint32_t y) noexcept;
    uint8_t WindowMask(uint32_t x, uint32_t y) const noexcept;
    uint16_t Compose(uint32_t x, uint32_t y) const noexcept;
    uint32_t ToAbgr(uint16_t color) const noexcept;

    VideoMemory& memory_;
    int engine_;
    uint8_t hidden_layers_ = 0;
    std::array<uint8_t, kRegisterBytes> regs_{};
    std::array<int32_t, 2> ref_x_{}; // BG2/BG3 internal affine reference points
    std::array<int32_t, 2> ref_y_{};
    std::array<std::array<int32_t, kWidth>, 4> bg_line_{};
    std::array<bool, 4> bg_active_{};
    std::array<ObjPixel, kWidth> obj_line_{};
    std::array<bool, kWidth> obj_window_{};
};

} // namespace emugbds::inds
