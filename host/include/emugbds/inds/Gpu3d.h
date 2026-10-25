#pragma once

// The DS 3D engine (GBATEK "DS 3D Video"): the geometry engine (command FIFO, matrices, lighting,
// vertices, polygon setup and clipping, box/position/vector tests) and the rendering engine (a
// whole-frame rasterizer with depth buffer, textures, blending, and the rear plane), whose picture
// engine A shows as BG0.
// shortcut: commands run as soon as they reach the FIFO (only SWAP_BUFFERS waits, for V-Blank), so
// the FIFO is empty except while a swap is pending; command timings are not modelled.
// shortcut: no fog, edge marking, anti-aliasing, or shadow polygons yet (shadow polygons are not
// drawn); wire-frame polygons draw a one-pixel outline.

#include "emugbds/inds/VideoMemory.h"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace emugbds::inds {

class Gpu3d {
public:
    static constexpr uint32_t kWidth = 256u;
    static constexpr uint32_t kHeight = 192u;
    static constexpr uint32_t kRegistersFirst = 0x04000320u;
    static constexpr uint32_t kRegistersLast = 0x040006A3u;

    explicit Gpu3d(const VideoMemory& video) noexcept : video_(video) { Reset(); }
    void Reset() noexcept;

    // 4000320h-40006A3h (word aligned; mask selects byte lanes).
    uint32_t ReadWord(uint32_t address) const noexcept;
    void WriteWord(uint32_t address, uint32_t value, uint32_t mask) noexcept;

    // Line 192: a pending SWAP_BUFFERS hands the new polygon list to the renderer.
    void StartVBlank() noexcept;
    // Line 214: the next frame's picture is rendered (`disp3dcnt` is DISP3DCNT, 4000060h). Without a
    // new list, the same polygons are drawn again.
    void RenderFrame(uint16_t disp3dcnt) noexcept { Render(disp3dcnt); }

    // The rendered picture: 15-bit colour and 5-bit alpha (0 = transparent) per pixel, top line first.
    uint16_t color(uint32_t x, uint32_t y) const noexcept { return color_[y * kWidth + x]; }
    uint8_t alpha(uint32_t x, uint32_t y) const noexcept { return alpha_[y * kWidth + x]; }

    // GXSTAT FIFO state, for the geometry FIFO IRQ and DMA.
    size_t fifo_entries() const noexcept { return fifo_.size(); }
    bool fifo_less_than_half() const noexcept { return fifo_.size() < 128u; }
    bool fifo_empty() const noexcept { return fifo_.empty(); }
    uint32_t irq_mode() const noexcept { return (gxstat_control_ >> 30u) & 3u; }

private:
    using Matrix = std::array<int32_t, 16>; // 4x4, row-major, 20.12 fixed point

    struct Vertex {
        int32_t x = 0, y = 0, z = 0, w = 0; // clip coordinates (20.12)
        int32_t r = 0, g = 0, b = 0;        // 6-bit colour
        int32_t s = 0, t = 0;               // texture coordinates (12.4)
    };
    struct Polygon {
        std::array<Vertex, 10> vertices{};
        uint32_t count = 0;
        uint32_t attr = 0;
        uint32_t texture = 0;  // TEXIMAGE_PARAM
        uint32_t palette = 0;  // PLTT_BASE
        std::array<uint32_t, 4> viewport{}; // x1, y1, x2, y2 when the polygon was defined
        bool translucent = false;
    };
    struct FifoEntry {
        uint8_t command = 0;
        uint32_t parameter = 0;
    };

    // Geometry
    void PushEntry(uint8_t command, uint32_t parameter) noexcept;
    void ProcessFifo() noexcept;
    void Execute(uint8_t command, const uint32_t* parameters) noexcept;
    void MatrixCommand(uint8_t command, const uint32_t* parameters) noexcept;
    void ApplyToCurrent(const Matrix& m, bool multiply, bool vector_too) noexcept;
    void UpdateClipMatrix() noexcept;
    void SubmitVertex(int32_t x, int32_t y, int32_t z) noexcept;
    void FinishPolygon(const std::array<Vertex, 4>& corners, uint32_t count) noexcept;
    void SetNormal(uint32_t parameter) noexcept;
    void BoxTest(const uint32_t* parameters) noexcept;
    static Matrix Multiply(const Matrix& a, const Matrix& b) noexcept;
    static Matrix Identity() noexcept;

    // Rendering
    void Render(uint16_t disp3dcnt) noexcept;
    void ClearFrame(uint16_t disp3dcnt) noexcept;
    void DrawPolygon(const Polygon& polygon, uint16_t disp3dcnt, bool w_buffer) noexcept;
    void TexelColor(const Polygon& polygon, int32_t s, int32_t t, int32_t& r, int32_t& g, int32_t& b, int32_t& a) const noexcept;

    const VideoMemory& video_;

    // Command FIFO and packed-command decoding
    std::deque<FifoEntry> fifo_;
    std::array<uint8_t, 4> packed_{};
    uint32_t packed_count_ = 0;   // commands left in the current packed word
    uint32_t packed_params_ = 0;  // parameters still expected for packed_[0]
    bool swap_pending_ = false;
    uint32_t swap_parameter_ = 0;

    // Matrices
    uint32_t matrix_mode_ = 0;
    Matrix projection_{}, position_{}, vector_{}, texture_{}, clip_{};
    Matrix projection_stack_{};
    std::array<Matrix, 32> position_stack_{};
    std::array<Matrix, 32> vector_stack_{};
    Matrix texture_stack_{};
    uint32_t position_sp_ = 0;
    uint32_t projection_sp_ = 0;
    uint32_t texture_sp_ = 0;
    bool stack_error_ = false;
    uint32_t gxstat_control_ = 0; // GXSTAT bits 30-31

    // Vertex state
    uint32_t polygon_attr_ = 0;       // POLYGON_ATTR as written
    uint32_t active_attr_ = 0;        // latched by BEGIN_VTXS
    uint32_t primitive_ = 0;
    uint32_t texture_param_ = 0;
    uint32_t palette_base_ = 0;
    std::array<int32_t, 3> vertex_color_{};    // 6-bit
    std::array<int32_t, 2> texcoord_{};        // as given (12.4)
    std::array<int32_t, 2> texcoord_out_{};    // after the texture matrix
    std::array<int16_t, 3> last_vertex_{};     // for VTX_XY/XZ/YZ/DIFF
    std::array<Vertex, 4> strip_{};
    uint32_t strip_count_ = 0;
    uint32_t polygon_index_in_strip_ = 0;
    std::array<uint32_t, 4> viewport_{0u, 0u, 255u, 191u};
    // Lighting
    std::array<std::array<int32_t, 3>, 4> light_vector_{};
    std::array<std::array<int32_t, 3>, 4> half_vector_{};
    std::array<std::array<int32_t, 3>, 4> light_color_{};
    std::array<int32_t, 3> diffuse_{}, ambient_{}, specular_{}, emission_{};
    bool shininess_enabled_ = false;
    std::array<uint8_t, 128> shininess_{};
    // Tests
    std::array<int32_t, 4> position_result_{};
    std::array<int16_t, 3> vector_result_{};
    bool box_result_ = false;

    // Polygon lists: the one being defined, and the one being rendered
    std::vector<Polygon> geometry_;
    std::vector<Polygon> rendering_;
    uint32_t geometry_vertices_ = 0;
    uint32_t geometry_flags_ = 0;   // swap parameter that applies to the list being defined
    uint32_t rendering_flags_ = 0;  // and to the list being rendered

    // Rendering registers (4000330h-40003BFh), the 1-dot depth (4000610h)
    std::array<uint8_t, 0x90> render_regs_{};
    uint16_t one_dot_depth_ = 0x7FFFu;

    // Frame buffer and per-pixel attributes
    std::array<uint16_t, kWidth * kHeight> color_{};
    std::array<uint8_t, kWidth * kHeight> alpha_{};
    std::vector<uint32_t> rgb6_;      // 6-bit R, G, B packed (R | G << 8 | B << 16)
    std::vector<uint8_t> alpha5_;
    std::vector<uint32_t> depth_;     // 24-bit
    std::vector<int8_t> translucent_id_; // polygon ID of the last translucent pixel, -1 if none
};

} // namespace emugbds::inds
