#pragma once

// GBA sound: Direct Sound FIFOs A/B mixed to 32768 Hz stereo. The four PSG
// channels are not implemented yet.

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::ingba {

inline constexpr uint32_t kSoundCntHAddress = 0x04000082u;
inline constexpr uint32_t kSoundCntXAddress = 0x04000084u;
inline constexpr uint32_t kFifoAAddress = 0x040000A0u;
inline constexpr uint32_t kFifoBAddress = 0x040000A4u;

class Apu {
public:
    static constexpr uint32_t kSampleRate = 32768u;
    static constexpr uint32_t kCyclesPerSample = 512u; // 16.78 MHz / 32768 Hz

    void Reset();
    void WriteFifo(uint32_t fifo, uint8_t value) noexcept;
    void ResetFifo(uint32_t fifo) noexcept;
    // Plays the next byte of every FIFO driven by this timer. Returns a bit
    // mask (bit 0 = A, bit 1 = B) of FIFOs that need a DMA refill.
    uint32_t OnTimerOverflow(uint32_t timer, uint16_t soundcnt_h) noexcept;
    // Emits one stereo sample per kCyclesPerSample elapsed cycles.
    void Advance(uint32_t cycles, uint16_t soundcnt_h, uint16_t soundcnt_x);

    // Interleaved left/right samples produced since the last ClearSamples().
    const std::vector<int16_t>& samples() const noexcept { return samples_; }
    void ClearSamples() noexcept { samples_.clear(); }

private:
    struct Fifo {
        std::array<int8_t, 32u> bytes{};
        uint32_t read = 0u;
        uint32_t count = 0u;
        int8_t output = 0;
    };

    std::array<Fifo, 2u> fifos_{};
    uint32_t sample_phase_ = 0u;
    std::vector<int16_t> samples_;
};

} // namespace emugbds::ingba
