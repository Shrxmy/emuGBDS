#include "emugbds/ingba/Apu.h"

#include <algorithm>

namespace emugbds::ingba {

void Apu::Reset() {
    fifos_ = {};
    sample_phase_ = 0u;
    samples_.clear();
    samples_.reserve(4096u); // Several frames of stereo output, so Advance rarely allocates.
}


void Apu::WriteFifo(uint32_t fifo, uint8_t value) noexcept {
    Fifo& f = fifos_[fifo & 1u];
    if (f.count == f.bytes.size()) {
        return; // A full FIFO drops further writes.
    }
    f.bytes[(f.read + f.count) % f.bytes.size()] = static_cast<int8_t>(value);
    ++f.count;
}


void Apu::ResetFifo(uint32_t fifo) noexcept {
    Fifo& f = fifos_[fifo & 1u];
    f.read = 0u;
    f.count = 0u;
}


uint32_t Apu::OnTimerOverflow(uint32_t timer, uint16_t soundcnt_h) noexcept {
    uint32_t refill = 0u;
    for (uint32_t fifo = 0u; fifo < 2u; ++fifo) {
        const uint32_t timer_select = (soundcnt_h >> (10u + fifo * 4u)) & 1u;
        if (timer_select != timer) {
            continue;
        }
        Fifo& f = fifos_[fifo];
        if (f.count != 0u) {
            f.output = f.bytes[f.read];
            f.read = (f.read + 1u) % f.bytes.size();
            --f.count;
        }
        if (f.count <= 16u) {
            refill |= 1u << fifo;
        }
    }
    return refill;
}


void Apu::Advance(uint32_t cycles, uint16_t soundcnt_h, uint16_t soundcnt_x) {
    sample_phase_ += cycles;
    while (sample_phase_ >= kCyclesPerSample) {
        sample_phase_ -= kCyclesPerSample;
        int32_t left = 0;
        int32_t right = 0;
        if ((soundcnt_x & 0x0080u) != 0u) {
            for (uint32_t fifo = 0u; fifo < 2u; ++fifo) {
                // Each FIFO spans +/-0x200 at 100% volume and half that at 50%.
                const int32_t level = fifos_[fifo].output * (((soundcnt_h >> (2u + fifo)) & 1u) != 0u ? 4 : 2);
                if (((soundcnt_h >> (8u + fifo * 4u)) & 1u) != 0u) {
                    right += level;
                }
                if (((soundcnt_h >> (9u + fifo * 4u)) & 1u) != 0u) {
                    left += level;
                }
            }
        }
        // shortcut: assumes the default SOUNDBIAS of 0x200, so the 10-bit output clips at -0x200..0x1FF.
        const auto to_pcm = [](int32_t level) {
            return static_cast<int16_t>(std::clamp(level, -0x200, 0x1FF) * 64);
        };
        samples_.push_back(to_pcm(left));
        samples_.push_back(to_pcm(right));
    }
}

} // namespace emugbds::ingba
