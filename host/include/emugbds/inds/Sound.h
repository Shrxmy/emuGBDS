#pragma once

// DS sound (ARM7 4000400h-400051Fh, GBATEK "DS Sound"): sixteen channels that
// play PCM8, PCM16 or IMA-ADPCM straight from memory, PSG square waves on
// channels 8-13 and noise on 14-15; the mixer with SOUNDCNT output routing,
// master volume and SOUNDBIAS; and the two capture units that write the mixer
// (or channel 0/2) back to memory. Output is stereo int16 at 32768 Hz.
// shortcut: channels are point-sampled once per output sample (the hardware
// mixes at 1.05 MHz), the ch(a)+ch(b) capture addition mode and the capture
// bugs are not emulated, and Manual repeat mode plays like Loop (GBATEK does
// not describe it).

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::inds {

class MemoryBus;

class Sound {
public:
    static constexpr uint32_t kSampleRate = 32768u;
    static constexpr uint32_t kArm7Clock = 33513982u;
    static constexpr uint32_t kRegisterBytes = 0x120u; // 4000400h-400051Fh

    explicit Sound(MemoryBus& memory) noexcept : memory_(memory) {}
    Sound(const Sound&) = delete;
    Sound& operator=(const Sound&) = delete;

    void Reset();
    // offset = address - 4000400h (word aligned); mask selects byte lanes.
    uint32_t ReadWord(uint32_t offset);
    void WriteWord(uint32_t offset, uint32_t value, uint32_t mask);
    // Runs the sound hardware for `cycles` ARM7 cycles.
    void Advance(uint32_t cycles);

    // Interleaved left/right samples produced since the last ClearSamples().
    const std::vector<int16_t>& samples() const noexcept { return samples_; }
    void ClearSamples() noexcept { samples_.clear(); }

private:
    struct Channel {
        uint32_t control = 0; // SOUNDxCNT; bit 31 is the live busy flag
        uint32_t source = 0;
        uint16_t timer = 0;
        uint16_t loop_start = 0; // words
        uint32_t length = 0;     // words
        bool active = false;
        bool ending = false;     // one-shot finished: the last sample plays out its period
        bool stalled = false;    // PNT+LEN under 4 words: busy forever, silent
        int32_t remaining = 0;   // ARM7 cycles to the next timer overflow
        uint32_t delay = 0;      // sample periods before the first real sample
        uint32_t position = 0;   // bytes (PCM), nibbles (ADPCM), or duty step (PSG)
        int32_t sample = 0;      // current PCM16 output
        int32_t adpcm = 0;
        int32_t adpcm_index = 0;
        int32_t loop_adpcm = 0;
        int32_t loop_adpcm_index = 0;
        uint16_t noise = 0;
    };

    struct Capture {
        uint8_t control = 0; // SNDCAPxCNT; bit 7 is the live busy flag
        uint32_t destination = 0;
        uint16_t length = 0; // words (0 acts as 1)
        bool active = false;
        int32_t remaining = 0;
        uint32_t position = 0; // bytes
    };

    static int32_t Period(uint16_t timer) noexcept { return (0x10000 - static_cast<int32_t>(timer)) * 2; }
    void Sync();
    void Run(uint32_t cycles);
    void RunChannels(uint32_t cycles);
    void Start(uint32_t index);
    void Step(uint32_t index);
    void FetchAdpcm(Channel& channel);
    int64_t Volume(uint32_t index) const noexcept;
    void Mixers(int64_t& left_sum, int64_t& right_sum, std::array<int64_t, 4>& routed) const noexcept;
    void Mix();
    void StepCapture(uint32_t index);

    MemoryBus& memory_;
    std::array<Channel, 16> channels_{};
    std::array<Capture, 2> captures_{};
    uint16_t control_ = 0; // SOUNDCNT
    uint16_t bias_ = 0;    // SOUNDBIAS
    uint32_t pending_ = 0; // ARM7 cycles not yet applied to the channels
    uint64_t phase_ = 0;   // output-sample clock, in kSampleRate units of ARM7 cycles
    std::vector<int16_t> samples_;
};

} // namespace emugbds::inds
