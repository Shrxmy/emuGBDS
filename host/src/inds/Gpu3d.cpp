#include "emugbds/inds/Gpu3d.h"

#include <algorithm>

// Geometry engine. Rendering is in Gpu3dRender.cpp.

namespace emugbds::inds {
namespace {

constexpr uint32_t kFifo = 0x04000400u;      // GXFIFO, mirrored to 400043Fh
constexpr uint32_t kPortsFirst = 0x04000440u; // command ports: 4000400h + command * 4
constexpr uint32_t kPortsLast = 0x040005FFu;
constexpr uint32_t kRenderRegsFirst = 0x04000330u;
constexpr uint32_t kGxStat = 0x04000600u;
constexpr size_t kMaxPolygons = 2048u;
constexpr uint32_t kMaxVertices = 6144u;

// Parameters per command (GBATEK "DS 3D I/O Map"); invalid commands take none and do nothing.
uint32_t ParameterCount(uint8_t command) noexcept {
    switch (command) {
    case 0x10u: case 0x12u: case 0x13u: case 0x14u: return 1u;
    case 0x16u: case 0x18u: return 16u;
    case 0x17u: case 0x19u: return 12u;
    case 0x1Au: return 9u;
    case 0x1Bu: case 0x1Cu: return 3u;
    case 0x20u: case 0x21u: case 0x22u: case 0x24u: case 0x25u: case 0x26u: case 0x27u: case 0x28u:
    case 0x29u: case 0x2Au: case 0x2Bu: case 0x30u: case 0x31u: case 0x32u: case 0x33u:
    case 0x40u: case 0x50u: case 0x60u: case 0x72u: return 1u;
    case 0x23u: case 0x71u: return 2u;
    case 0x34u: return 32u;
    case 0x70u: return 3u;
    default: return 0u;
    }
}

int32_t SignExtend(uint32_t value, uint32_t bits) noexcept {
    return static_cast<int32_t>(value << (32u - bits)) >> (32u - bits);
}

// 5-bit colour channel to the 6-bit form the 3D engine works with (GBATEK "Polygon Attributes").
int32_t Expand5(uint32_t channel) noexcept {
    return channel == 0u ? 0 : static_cast<int32_t>(channel * 2u + 1u);
}

std::array<int32_t, 3> Color6(uint32_t color) noexcept {
    return {Expand5(color & 31u), Expand5((color >> 5u) & 31u), Expand5((color >> 10u) & 31u)};
}

// A 1+9 fractional vector component (10 bits) as 12-bit fraction.
int32_t Vector10(uint32_t value) noexcept {
    return SignExtend(value & 0x3FFu, 10u) * 8;
}

} // namespace

void Gpu3d::Reset() noexcept {
    fifo_.clear();
    packed_ = {};
    packed_count_ = 0u;
    packed_params_ = 0u;
    swap_pending_ = false;
    swap_parameter_ = 0u;
    matrix_mode_ = 0u;
    projection_ = position_ = vector_ = texture_ = clip_ = Identity();
    projection_stack_ = texture_stack_ = Identity();
    position_stack_.fill(Identity());
    vector_stack_.fill(Identity());
    position_sp_ = projection_sp_ = texture_sp_ = 0u;
    stack_error_ = false;
    gxstat_control_ = 0u;
    polygon_attr_ = active_attr_ = 0u;
    primitive_ = 0u;
    texture_param_ = palette_base_ = 0u;
    vertex_color_ = {63, 63, 63};
    texcoord_ = texcoord_out_ = {};
    last_vertex_ = {};
    strip_count_ = polygon_index_in_strip_ = 0u;
    viewport_ = {0u, 0u, 255u, 191u};
    light_vector_ = half_vector_ = light_color_ = {};
    diffuse_ = ambient_ = specular_ = emission_ = {};
    shininess_enabled_ = false;
    shininess_.fill(0u);
    position_result_ = {};
    vector_result_ = {};
    box_result_ = false;
    geometry_.clear();
    rendering_.clear();
    geometry_vertices_ = 0u;
    geometry_flags_ = rendering_flags_ = 0u;
    render_regs_.fill(0u);
    one_dot_depth_ = 0x7FFFu;
    color_.fill(0u);
    alpha_.fill(0u);
    rgb6_.assign(kWidth * kHeight, 0u);
    alpha5_.assign(kWidth * kHeight, 0u);
    depth_.assign(kWidth * kHeight, 0u);
    translucent_id_.assign(kWidth * kHeight, -1);
}


uint32_t Gpu3d::ReadWord(uint32_t address) const noexcept {
    if (address == 0x04000320u) {
        return 46u; // RDLINES_COUNT: the renderer is never behind
    }
    if (address == kGxStat) {
        const uint32_t entries = static_cast<uint32_t>(std::min<size_t>(fifo_.size(), 256u));
        return (box_result_ ? 1u << 1 : 0u) | ((position_sp_ & 31u) << 8u) | ((projection_sp_ & 1u) << 13u) |
            (stack_error_ ? 1u << 15 : 0u) | (entries << 16u) | (fifo_less_than_half() ? 1u << 25 : 0u) |
            (fifo_empty() ? 1u << 26 : 0u) | ((swap_pending_ || !fifo_.empty()) ? 1u << 27 : 0u) | gxstat_control_;
    }
    if (address == 0x04000604u) { // RAM_COUNT
        return static_cast<uint32_t>(geometry_.size()) | (geometry_vertices_ << 16u);
    }
    if (address >= 0x04000620u && address < 0x04000630u) {
        return static_cast<uint32_t>(position_result_[(address - 0x04000620u) / 4u]);
    }
    if (address >= 0x04000630u && address < 0x04000636u) {
        const uint32_t index = (address - 0x04000630u) / 2u;
        const uint32_t low = static_cast<uint16_t>(vector_result_[index]);
        const uint32_t high = index + 1u < 3u ? static_cast<uint16_t>(vector_result_[index + 1u]) : 0u;
        return low | (high << 16u);
    }
    if (address >= 0x04000640u && address < 0x04000680u) {
        return static_cast<uint32_t>(clip_[(address - 0x04000640u) / 4u]);
    }
    if (address >= 0x04000680u && address < 0x040006A4u) {
        const uint32_t index = (address - 0x04000680u) / 4u; // 3x3: rows 0-2, columns 0-2
        return static_cast<uint32_t>(vector_[(index / 3u) * 4u + index % 3u]);
    }
    return 0u;
}


void Gpu3d::WriteWord(uint32_t address, uint32_t value, uint32_t mask) noexcept {
    if (address >= kRenderRegsFirst && address < kRenderRegsFirst + render_regs_.size()) {
        for (uint32_t lane = 0u; lane < 4u; ++lane) {
            if ((mask & (0xFFu << (lane * 8u))) != 0u) {
                render_regs_[address - kRenderRegsFirst + lane] = static_cast<uint8_t>(value >> (lane * 8u));
            }
        }
        return;
    }
    if (address == kGxStat) {
        if ((mask & 0xC0000000u) != 0u) {
            gxstat_control_ = value & 0xC0000000u;
        }
        if ((mask & value & (1u << 15)) != 0u) { // acknowledge a stack error
            stack_error_ = false;
            projection_sp_ = 0u;
        }
        return;
    }
    if (address == 0x04000610u) {
        one_dot_depth_ = static_cast<uint16_t>(value & 0x7FFFu);
        return;
    }
    if (address >= kFifo && address < kPortsFirst) {
        // GXFIFO: a word of up to four packed commands, then their parameters in order.
        if (packed_count_ == 0u) {
            for (uint32_t i = 0u; i < 4u; ++i) {
                packed_[i] = static_cast<uint8_t>(value >> (i * 8u));
            }
            packed_count_ = 4u;
        } else {
            PushEntry(packed_[4u - packed_count_], value);
            if (--packed_params_ != 0u) {
                return;
            }
            --packed_count_;
        }
        while (packed_count_ != 0u) {
            const uint8_t command = packed_[4u - packed_count_];
            packed_params_ = ParameterCount(command);
            if (packed_params_ != 0u) {
                return; // wait for its parameters
            }
            if (command != 0u) {
                PushEntry(command, 0u);
            }
            --packed_count_;
        }
        return;
    }
    if (address >= kPortsFirst && address <= kPortsLast) {
        PushEntry(static_cast<uint8_t>((address - kFifo) / 4u), value);
    }
}


void Gpu3d::PushEntry(uint8_t command, uint32_t parameter) noexcept {
    // shortcut: the FIFO never fills up (writes that would stall the CPU on hardware just queue).
    fifo_.push_back({command, parameter});
    ProcessFifo();
}


void Gpu3d::ProcessFifo() noexcept {
    std::array<uint32_t, 32> parameters{};
    while (!swap_pending_ && !fifo_.empty()) {
        const uint8_t command = fifo_.front().command;
        const uint32_t count = ParameterCount(command);
        if (fifo_.size() < std::max(count, 1u)) {
            return; // parameters still to come
        }
        for (uint32_t i = 0u; i < std::max(count, 1u); ++i) {
            parameters[i] = fifo_.front().parameter;
            fifo_.pop_front();
        }
        Execute(command, parameters.data());
    }
}


void Gpu3d::StartVBlank() noexcept {
    if (swap_pending_) {
        // The list defined so far goes to the renderer with the flags of the previous swap.
        rendering_.swap(geometry_);
        geometry_.clear();
        geometry_vertices_ = 0u;
        rendering_flags_ = geometry_flags_;
        geometry_flags_ = swap_parameter_ & 3u;
        swap_pending_ = false;
        ProcessFifo();
    }
}


Gpu3d::Matrix Gpu3d::Identity() noexcept {
    Matrix m{};
    m[0] = m[5] = m[10] = m[15] = 0x1000;
    return m;
}


Gpu3d::Matrix Gpu3d::Multiply(const Matrix& a, const Matrix& b) noexcept {
    Matrix result{};
    for (uint32_t row = 0u; row < 4u; ++row) {
        for (uint32_t column = 0u; column < 4u; ++column) {
            int64_t sum = 0;
            for (uint32_t k = 0u; k < 4u; ++k) {
                sum += static_cast<int64_t>(a[row * 4u + k]) * b[k * 4u + column];
            }
            result[row * 4u + column] = static_cast<int32_t>(sum >> 12);
        }
    }
    return result;
}


void Gpu3d::UpdateClipMatrix() noexcept {
    clip_ = Multiply(position_, projection_);
}


void Gpu3d::ApplyToCurrent(const Matrix& m, bool multiply, bool vector_too) noexcept {
    // C = M * C, or C = M for loads.
    switch (matrix_mode_) {
    case 0u:
        projection_ = multiply ? Multiply(m, projection_) : m;
        break;
    case 1u:
    case 2u:
        position_ = multiply ? Multiply(m, position_) : m;
        if (matrix_mode_ == 2u && vector_too) {
            vector_ = multiply ? Multiply(m, vector_) : m;
        }
        break;
    default:
        texture_ = multiply ? Multiply(m, texture_) : m;
        return;
    }
    UpdateClipMatrix();
}


void Gpu3d::MatrixCommand(uint8_t command, const uint32_t* p) noexcept {
    Matrix m = Identity();
    const auto fill = [&](uint32_t rows, uint32_t columns) {
        for (uint32_t row = 0u; row < rows; ++row) {
            for (uint32_t column = 0u; column < columns; ++column) {
                m[row * 4u + column] = static_cast<int32_t>(p[row * columns + column]);
            }
        }
    };
    switch (command) {
    case 0x15u: // MTX_IDENTITY
        ApplyToCurrent(m, false, true);
        break;
    case 0x16u: // MTX_LOAD_4x4
    case 0x18u: // MTX_MULT_4x4
        fill(4u, 4u);
        ApplyToCurrent(m, command == 0x18u, true);
        break;
    case 0x17u: // MTX_LOAD_4x3
    case 0x19u: // MTX_MULT_4x3
        fill(4u, 3u);
        ApplyToCurrent(m, command == 0x19u, true);
        break;
    case 0x1Au: // MTX_MULT_3x3
        fill(3u, 3u);
        ApplyToCurrent(m, true, true);
        break;
    case 0x1Bu: // MTX_SCALE: the vector matrix keeps its length, even in mode 2
        m[0] = static_cast<int32_t>(p[0]);
        m[5] = static_cast<int32_t>(p[1]);
        m[10] = static_cast<int32_t>(p[2]);
        ApplyToCurrent(m, true, false);
        break;
    case 0x1Cu: // MTX_TRANS
        m[12] = static_cast<int32_t>(p[0]);
        m[13] = static_cast<int32_t>(p[1]);
        m[14] = static_cast<int32_t>(p[2]);
        ApplyToCurrent(m, true, true);
        break;
    default:
        break;
    }
}


void Gpu3d::Execute(uint8_t command, const uint32_t* p) noexcept {
    switch (command) {
    case 0x10u: // MTX_MODE
        matrix_mode_ = p[0] & 3u;
        return;
    case 0x11u: // MTX_PUSH; the stack commands treat mode 1 like mode 2
        if (matrix_mode_ == 0u) {
            stack_error_ = stack_error_ || projection_sp_ != 0u;
            projection_stack_ = projection_;
            projection_sp_ = (projection_sp_ + 1u) & 1u;
        } else if (matrix_mode_ == 3u) {
            texture_stack_ = texture_;
            texture_sp_ = (texture_sp_ + 1u) & 1u;
        } else {
            stack_error_ = stack_error_ || position_sp_ >= 31u;
            position_stack_[position_sp_ & 31u] = position_;
            vector_stack_[position_sp_ & 31u] = vector_;
            position_sp_ = (position_sp_ + 1u) & 63u;
        }
        return;
    case 0x12u: // MTX_POP
        if (matrix_mode_ == 0u) {
            projection_sp_ = (projection_sp_ - 1u) & 1u;
            stack_error_ = stack_error_ || projection_sp_ != 0u;
            projection_ = projection_stack_;
            UpdateClipMatrix();
        } else if (matrix_mode_ == 3u) {
            texture_sp_ = (texture_sp_ - 1u) & 1u;
            texture_ = texture_stack_;
        } else {
            position_sp_ = static_cast<uint32_t>(static_cast<int32_t>(position_sp_) - SignExtend(p[0] & 63u, 6u)) & 63u;
            stack_error_ = stack_error_ || position_sp_ >= 31u;
            position_ = position_stack_[position_sp_ & 31u];
            vector_ = vector_stack_[position_sp_ & 31u];
            UpdateClipMatrix();
        }
        return;
    case 0x13u: // MTX_STORE
    case 0x14u: // MTX_RESTORE
        if (matrix_mode_ == 0u) {
            (command == 0x13u ? projection_stack_ : projection_) = command == 0x13u ? projection_ : projection_stack_;
            UpdateClipMatrix();
        } else if (matrix_mode_ == 3u) {
            (command == 0x13u ? texture_stack_ : texture_) = command == 0x13u ? texture_ : texture_stack_;
        } else {
            const uint32_t index = p[0] & 31u;
            stack_error_ = stack_error_ || index == 31u;
            if (command == 0x13u) {
                position_stack_[index] = position_;
                vector_stack_[index] = vector_;
            } else {
                position_ = position_stack_[index];
                vector_ = vector_stack_[index];
                UpdateClipMatrix();
            }
        }
        return;
    case 0x15u: case 0x16u: case 0x17u: case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu: case 0x1Cu:
        MatrixCommand(command, p);
        return;
    case 0x20u: // COLOR
        vertex_color_ = Color6(p[0]);
        return;
    case 0x21u: // NORMAL
        SetNormal(p[0]);
        return;
    case 0x22u: { // TEXCOORD
        texcoord_ = {SignExtend(p[0] & 0xFFFFu, 16u), SignExtend(p[0] >> 16u, 16u)};
        texcoord_out_ = texcoord_;
        if ((texture_param_ >> 30u) == 1u) { // TexCoord source: (S, T, 1/16, 1/16) x texture matrix
            for (uint32_t i = 0u; i < 2u; ++i) {
                const int64_t sum = static_cast<int64_t>(texcoord_[0]) * texture_[i] +
                    static_cast<int64_t>(texcoord_[1]) * texture_[4u + i] + texture_[8u + i] + texture_[12u + i];
                texcoord_out_[i] = SignExtend(static_cast<uint32_t>(sum >> 12), 16u);
            }
        }
        return;
    }
    case 0x23u: // VTX_16
        SubmitVertex(SignExtend(p[0] & 0xFFFFu, 16u), SignExtend(p[0] >> 16u, 16u), SignExtend(p[1] & 0xFFFFu, 16u));
        return;
    case 0x24u: // VTX_10
        SubmitVertex(SignExtend(p[0] & 0x3FFu, 10u) * 64, SignExtend((p[0] >> 10u) & 0x3FFu, 10u) * 64,
            SignExtend((p[0] >> 20u) & 0x3FFu, 10u) * 64);
        return;
    case 0x25u: // VTX_XY
        SubmitVertex(SignExtend(p[0] & 0xFFFFu, 16u), SignExtend(p[0] >> 16u, 16u), last_vertex_[2]);
        return;
    case 0x26u: // VTX_XZ
        SubmitVertex(SignExtend(p[0] & 0xFFFFu, 16u), last_vertex_[1], SignExtend(p[0] >> 16u, 16u));
        return;
    case 0x27u: // VTX_YZ
        SubmitVertex(last_vertex_[0], SignExtend(p[0] & 0xFFFFu, 16u), SignExtend(p[0] >> 16u, 16u));
        return;
    case 0x28u: // VTX_DIFF: 10-bit offsets in 1/4096 units
        SubmitVertex(last_vertex_[0] + SignExtend(p[0] & 0x3FFu, 10u), last_vertex_[1] + SignExtend((p[0] >> 10u) & 0x3FFu, 10u),
            last_vertex_[2] + SignExtend((p[0] >> 20u) & 0x3FFu, 10u));
        return;
    case 0x29u: // POLYGON_ATTR (applied by the next BEGIN_VTXS)
        polygon_attr_ = p[0];
        return;
    case 0x2Au:
        texture_param_ = p[0];
        return;
    case 0x2Bu:
        palette_base_ = p[0] & 0x1FFFu;
        return;
    case 0x30u: // DIF_AMB
        diffuse_ = {static_cast<int32_t>(p[0] & 31u), static_cast<int32_t>((p[0] >> 5u) & 31u), static_cast<int32_t>((p[0] >> 10u) & 31u)};
        ambient_ = {static_cast<int32_t>((p[0] >> 16u) & 31u), static_cast<int32_t>((p[0] >> 21u) & 31u), static_cast<int32_t>((p[0] >> 26u) & 31u)};
        if ((p[0] & 0x8000u) != 0u) {
            vertex_color_ = Color6(p[0]);
        }
        return;
    case 0x31u: // SPE_EMI
        specular_ = {static_cast<int32_t>(p[0] & 31u), static_cast<int32_t>((p[0] >> 5u) & 31u), static_cast<int32_t>((p[0] >> 10u) & 31u)};
        emission_ = {static_cast<int32_t>((p[0] >> 16u) & 31u), static_cast<int32_t>((p[0] >> 21u) & 31u), static_cast<int32_t>((p[0] >> 26u) & 31u)};
        shininess_enabled_ = (p[0] & 0x8000u) != 0u;
        return;
    case 0x32u: { // LIGHT_VECTOR, rotated by the vector matrix
        const uint32_t light = p[0] >> 30u;
        const int32_t in[3] = {Vector10(p[0]), Vector10(p[0] >> 10u), Vector10(p[0] >> 20u)};
        for (uint32_t i = 0u; i < 3u; ++i) {
            const int64_t sum = static_cast<int64_t>(in[0]) * vector_[i] + static_cast<int64_t>(in[1]) * vector_[4u + i] +
                static_cast<int64_t>(in[2]) * vector_[8u + i];
            light_vector_[light][i] = static_cast<int32_t>(sum >> 12);
        }
        // Half of light + line of sight (0, 0, -1.0).
        half_vector_[light] = {light_vector_[light][0] / 2, light_vector_[light][1] / 2, (light_vector_[light][2] - 0x1000) / 2};
        return;
    }
    case 0x33u: // LIGHT_COLOR
        light_color_[p[0] >> 30u] = {static_cast<int32_t>(p[0] & 31u), static_cast<int32_t>((p[0] >> 5u) & 31u),
            static_cast<int32_t>((p[0] >> 10u) & 31u)};
        return;
    case 0x34u: // SHININESS
        for (uint32_t i = 0u; i < 32u; ++i) {
            for (uint32_t byte = 0u; byte < 4u; ++byte) {
                shininess_[i * 4u + byte] = static_cast<uint8_t>(p[i] >> (byte * 8u));
            }
        }
        return;
    case 0x40u: // BEGIN_VTXS
        primitive_ = p[0] & 3u;
        active_attr_ = polygon_attr_;
        strip_count_ = 0u;
        polygon_index_in_strip_ = 0u;
        return;
    case 0x50u: // SWAP_BUFFERS: the geometry engine halts until V-Blank
        swap_pending_ = true;
        swap_parameter_ = p[0];
        return;
    case 0x60u: // VIEWPORT
        viewport_ = {p[0] & 0xFFu, (p[0] >> 8u) & 0xFFu, (p[0] >> 16u) & 0xFFu, p[0] >> 24u};
        return;
    case 0x70u:
        BoxTest(p);
        return;
    case 0x71u: { // POS_TEST: also becomes the last vertex for relative VTX commands
        last_vertex_ = {static_cast<int16_t>(p[0]), static_cast<int16_t>(p[0] >> 16u), static_cast<int16_t>(p[1])};
        const int64_t in[4] = {last_vertex_[0], last_vertex_[1], last_vertex_[2], 0x1000};
        for (uint32_t i = 0u; i < 4u; ++i) {
            int64_t sum = 0;
            for (uint32_t k = 0u; k < 4u; ++k) {
                sum += in[k] * clip_[k * 4u + i];
            }
            position_result_[i] = static_cast<int32_t>(sum >> 12);
        }
        return;
    }
    case 0x72u: { // VEC_TEST
        const int32_t in[3] = {Vector10(p[0]), Vector10(p[0] >> 10u), Vector10(p[0] >> 20u)};
        for (uint32_t i = 0u; i < 3u; ++i) {
            const int64_t sum = static_cast<int64_t>(in[0]) * vector_[i] + static_cast<int64_t>(in[1]) * vector_[4u + i] +
                static_cast<int64_t>(in[2]) * vector_[8u + i];
            vector_result_[i] = static_cast<int16_t>(SignExtend(static_cast<uint32_t>(sum >> 12) & 0xFFFFu, 13u));
        }
        return;
    }
    default:
        return; // NOP, END_VTXS, and invalid commands
    }
}


void Gpu3d::SetNormal(uint32_t parameter) noexcept {
    const int32_t normal[3] = {Vector10(parameter), Vector10(parameter >> 10u), Vector10(parameter >> 20u)};
    if ((texture_param_ >> 30u) == 2u) { // Normal source: (Nx, Ny, Nz, 1.0) x texture matrix, S/T as the last row
        for (uint32_t i = 0u; i < 2u; ++i) {
            const int64_t sum = static_cast<int64_t>(normal[0]) * texture_[i] + static_cast<int64_t>(normal[1]) * texture_[4u + i] +
                static_cast<int64_t>(normal[2]) * texture_[8u + i];
            texcoord_out_[i] = SignExtend(static_cast<uint32_t>((sum >> 20) + texcoord_[i]), 16u);
        }
    }
    int32_t rotated[3] = {};
    for (uint32_t i = 0u; i < 3u; ++i) {
        const int64_t sum = static_cast<int64_t>(normal[0]) * vector_[i] + static_cast<int64_t>(normal[1]) * vector_[4u + i] +
            static_cast<int64_t>(normal[2]) * vector_[8u + i];
        rotated[i] = static_cast<int32_t>(sum >> 12);
    }
    // GBATEK "Internal Operation on Normal Command", in 5-bit colour units with 12-bit fractions.
    int64_t color[3] = {static_cast<int64_t>(emission_[0]) << 12, static_cast<int64_t>(emission_[1]) << 12,
        static_cast<int64_t>(emission_[2]) << 12};
    for (uint32_t light = 0u; light < 4u; ++light) {
        if ((active_attr_ & (1u << light)) == 0u) {
            continue;
        }
        const auto dot = [&](const std::array<int32_t, 3>& v) {
            return (static_cast<int64_t>(v[0]) * rotated[0] + static_cast<int64_t>(v[1]) * rotated[1] +
                       static_cast<int64_t>(v[2]) * rotated[2]) >> 12;
        };
        const int64_t diffuse = std::max<int64_t>(0, -dot(light_vector_[light]));
        int64_t shine = std::max<int64_t>(0, -dot(half_vector_[light]));
        shine = std::min<int64_t>((shine * shine) >> 12, 0x1000);
        if (shininess_enabled_) {
            shine = static_cast<int64_t>(shininess_[static_cast<uint32_t>(std::min<int64_t>(shine >> 5, 127))]) << 4;
        }
        for (uint32_t c = 0u; c < 3u; ++c) {
            const int64_t light_color = light_color_[light][c];
            color[c] += (specular_[c] * light_color * shine) / 31;
            color[c] += (diffuse_[c] * light_color * diffuse) / 31;
            color[c] += (ambient_[c] * light_color << 12) / 31;
        }
    }
    for (uint32_t c = 0u; c < 3u; ++c) {
        vertex_color_[c] = Expand5(static_cast<uint32_t>(std::clamp<int64_t>(color[c] >> 12, 0, 31)));
    }
}


void Gpu3d::SubmitVertex(int32_t x, int32_t y, int32_t z) noexcept {
    last_vertex_ = {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z)};
    if ((texture_param_ >> 30u) == 3u) { // Vertex source: (Vx, Vy, Vz, 1.0) x texture matrix, S/T as the last row
        for (uint32_t i = 0u; i < 2u; ++i) {
            const int64_t sum = static_cast<int64_t>(x) * texture_[i] + static_cast<int64_t>(y) * texture_[4u + i] +
                static_cast<int64_t>(z) * texture_[8u + i];
            texcoord_out_[i] = SignExtend(static_cast<uint32_t>((sum >> 20) + texcoord_[i]), 16u); // 24 -> 4 fraction bits
        }
    }
    Vertex vertex;
    const int64_t in[4] = {x, y, z, 0x1000};
    int32_t out[4] = {};
    for (uint32_t i = 0u; i < 4u; ++i) {
        int64_t sum = 0;
        for (uint32_t k = 0u; k < 4u; ++k) {
            sum += in[k] * clip_[k * 4u + i];
        }
        out[i] = static_cast<int32_t>(sum >> 12);
    }
    vertex.x = out[0];
    vertex.y = out[1];
    vertex.z = out[2];
    vertex.w = out[3];
    vertex.r = vertex_color_[0];
    vertex.g = vertex_color_[1];
    vertex.b = vertex_color_[2];
    vertex.s = texcoord_out_[0];
    vertex.t = texcoord_out_[1];

    strip_[strip_count_++] = vertex;
    switch (primitive_) {
    case 0u: // separate triangles
        if (strip_count_ == 3u) {
            FinishPolygon(strip_, 3u);
            strip_count_ = 0u;
        }
        break;
    case 1u: // separate quads
        if (strip_count_ == 4u) {
            FinishPolygon(strip_, 4u);
            strip_count_ = 0u;
        }
        break;
    case 2u: // triangle strip: every second triangle is clockwise
        if (strip_count_ == 3u) {
            std::array<Vertex, 4> corners = strip_;
            if ((polygon_index_in_strip_ & 1u) != 0u) {
                std::swap(corners[0], corners[1]);
            }
            FinishPolygon(corners, 3u);
            ++polygon_index_in_strip_;
            strip_[0] = strip_[1];
            strip_[1] = strip_[2];
            strip_count_ = 2u;
        }
        break;
    default: // quad strip: v0 v1 v2 v3 form the quad v0 v1 v3 v2
        if (strip_count_ == 4u) {
            const std::array<Vertex, 4> corners = {strip_[0], strip_[1], strip_[3], strip_[2]};
            FinishPolygon(corners, 4u);
            strip_[0] = strip_[2];
            strip_[1] = strip_[3];
            strip_count_ = 2u;
        }
        break;
    }
}


void Gpu3d::FinishPolygon(const std::array<Vertex, 4>& corners, uint32_t count) noexcept {
    const uint32_t attr = active_attr_;
    if (((attr >> 4u) & 3u) == 3u) {
        return; // shortcut: shadow polygons are not drawn
    }
    if (geometry_.size() >= kMaxPolygons) {
        return; // Polygon RAM full
    }
    if ((attr & (1u << 12)) == 0u) { // far-plane intersecting polygons are hidden
        for (uint32_t i = 0u; i < count; ++i) {
            if (corners[i].z > corners[i].w) {
                return;
            }
        }
    }

    // Clip against the six planes of the view volume (-w <= x, y, z <= w), in clip space, where
    // colour and texture coordinates interpolate linearly.
    std::array<Vertex, 10> polygon{};
    std::array<Vertex, 10> next{};
    uint32_t size = count;
    std::copy(corners.begin(), corners.begin() + count, polygon.begin());
    for (uint32_t plane = 0u; plane < 6u && size != 0u; ++plane) {
        const auto distance = [plane](const Vertex& v) -> int64_t {
            const int64_t coordinate = plane < 2u ? v.x : plane < 4u ? v.y : v.z;
            return (plane & 1u) == 0u ? static_cast<int64_t>(v.w) - coordinate : static_cast<int64_t>(v.w) + coordinate;
        };
        uint32_t out = 0u;
        for (uint32_t i = 0u; i < size; ++i) {
            const Vertex& a = polygon[i];
            const Vertex& b = polygon[(i + 1u) % size];
            const int64_t da = distance(a);
            const int64_t db = distance(b);
            if (da >= 0 && out < next.size()) {
                next[out++] = a;
            }
            if ((da >= 0) != (db >= 0) && out < next.size()) {
                const double t = static_cast<double>(da) / static_cast<double>(da - db);
                const auto lerp = [t](int32_t from, int32_t to) {
                    return static_cast<int32_t>(from + (static_cast<double>(to) - from) * t);
                };
                Vertex v;
                v.x = lerp(a.x, b.x);
                v.y = lerp(a.y, b.y);
                v.z = lerp(a.z, b.z);
                v.w = lerp(a.w, b.w);
                v.r = lerp(a.r, b.r);
                v.g = lerp(a.g, b.g);
                v.b = lerp(a.b, b.b);
                v.s = lerp(a.s, b.s);
                v.t = lerp(a.t, b.t);
                next[out++] = v;
            }
        }
        polygon = next;
        size = out;
    }
    if (size < 3u) {
        return; // entirely outside the view volume
    }
    if (geometry_vertices_ + size > kMaxVertices) {
        return; // Vertex RAM full (counted after clipping, which can add vertices)
    }

    // Front faces are anticlockwise on screen (y up); line segments have no sides.
    double area = 0.0;
    for (uint32_t i = 0u; i < size; ++i) {
        const Vertex& a = polygon[i];
        const Vertex& b = polygon[(i + 1u) % size];
        const double ax = static_cast<double>(a.x) / std::max(a.w, 1);
        const double ay = static_cast<double>(a.y) / std::max(a.w, 1);
        const double bx = static_cast<double>(b.x) / std::max(b.w, 1);
        const double by = static_cast<double>(b.y) / std::max(b.w, 1);
        area += ax * by - bx * ay;
    }
    if ((area > 0.0 && (attr & (1u << 7)) == 0u) || (area < 0.0 && (attr & (1u << 6)) == 0u)) {
        return;
    }

    Polygon stored;
    std::copy(polygon.begin(), polygon.begin() + size, stored.vertices.begin());
    stored.count = size;
    stored.attr = attr;
    stored.texture = texture_param_;
    stored.palette = palette_base_;
    stored.viewport = viewport_;
    const uint32_t alpha = (attr >> 16u) & 31u;
    const uint32_t format = (texture_param_ >> 26u) & 7u;
    stored.translucent = (alpha != 0u && alpha != 31u) || format == 1u || format == 6u;
    geometry_.push_back(stored);
    geometry_vertices_ += size;
}


void Gpu3d::BoxTest(const uint32_t* p) noexcept {
    // True unless every corner is outside the same plane of the view volume (a conservative test).
    const int32_t origin[3] = {SignExtend(p[0] & 0xFFFFu, 16u), SignExtend(p[0] >> 16u, 16u), SignExtend(p[1] & 0xFFFFu, 16u)};
    const int32_t extent[3] = {SignExtend(p[1] >> 16u, 16u), SignExtend(p[2] & 0xFFFFu, 16u), SignExtend(p[2] >> 16u, 16u)};
    uint32_t outside_all = 0x3Fu;
    for (uint32_t corner = 0u; corner < 8u; ++corner) {
        int64_t in[4] = {origin[0] + ((corner & 1u) != 0u ? extent[0] : 0), origin[1] + ((corner & 2u) != 0u ? extent[1] : 0),
            origin[2] + ((corner & 4u) != 0u ? extent[2] : 0), 0x1000};
        int64_t out[4] = {};
        for (uint32_t i = 0u; i < 4u; ++i) {
            for (uint32_t k = 0u; k < 4u; ++k) {
                out[i] += in[k] * clip_[k * 4u + i];
            }
            out[i] >>= 12;
        }
        uint32_t outside = 0u;
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
            outside |= (out[axis] > out[3] ? 1u : 0u) << (axis * 2u);
            outside |= (out[axis] < -out[3] ? 1u : 0u) << (axis * 2u + 1u);
        }
        outside_all &= outside;
    }
    box_result_ = outside_all == 0u;
}

} // namespace emugbds::inds
