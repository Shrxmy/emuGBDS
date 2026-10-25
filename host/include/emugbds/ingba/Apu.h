#pragma once

// GBA sound (GBATEK "GBA Sound Controller"): the four PSG channels (two square
// waves, channel 1 with sweep; wave RAM; noise) with length, envelope and sweep
// clocked by a 512 Hz frame sequencer, plus Direct Sound FIFOs A/B, mixed to
// 32768 Hz stereo. PSG channel levels are averaged over each output sample.
// shortcut: wave RAM is modelled as two plain banks, not the hardware's shift
// register, and SOUNDBIAS is assumed to be the default 200h.

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::ingba {

inline constexpr uint32_t kPsgFirstAddress = 0x04000060u; // SOUND1CNT_L
inline constexpr uint32_t kPsgLastAddress = 0x04000081u;  // SOUNDCNT_L high byte
inline constexpr uint32_t kSoundCntLAddress = 0x04000080u;
inline constexpr uint32_t kSoundCntHAddress = 0x04000082u;
inline constexpr uint32_t kSoundCntXAddress = 0x04000084u;
inline constexpr uint32_t kWaveRamAddress = 0x04000090u;
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
    void Advance(uint32_t cycles, uint16_t soundcnt_l, uint16_t soundcnt_h, uint16_t soundcnt_x);

    // A byte written to 4000060h-4000081h (the bus keeps the register copy that reads return).
    void WritePsg(uint32_t address, uint8_t value);
    // SOUNDCNT_X bit 7 cleared: every PSG channel stops.
    void PowerOffPsg();
    // SOUNDCNT_X bits 0-3: channels still playing.
    uint8_t active_channels() const noexcept;
    // Wave RAM at 4000090h-400009Fh: the CPU sees the bank that is not playing.
    uint8_t ReadWave(uint32_t index) const noexcept;
    void WriteWave(uint32_t index, uint8_t value);

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

    struct Channel {
        bool on = false;
        uint32_t length = 0u;      // length clocks left
        int32_t timer = 0;         // cycles until the waveform steps
        uint32_t position = 0u;    // duty step (square) or wave digit
        int32_t volume = 0;        // envelope level 0-15
        uint32_t envelope_timer = 0u;
        int32_t accumulated = 0;   // level x cycles since the last output sample
    };

    uint8_t Reg(uint32_t offset) const noexcept { return psg_[offset]; }
    uint32_t Frequency(uint32_t channel) const noexcept;
    int32_t Period(uint32_t channel) const noexcept;
    int32_t Level(uint32_t channel) const noexcept;
    void Trigger(uint32_t channel);
    void StepWaveform(uint32_t channel);
    void ClockSequencer();
    void SyncPsg();
    void RunPsg(uint32_t cycles);
    void Mix(uint16_t soundcnt_l, uint16_t soundcnt_h, uint16_t soundcnt_x);

    std::array<Fifo, 2u> fifos_{};
    std::array<uint8_t, 0x22u> psg_{}; // 4000060h-4000081h as written
    std::array<Channel, 4u> channels_{};
    std::array<uint8_t, 32u> wave_ram_{}; // two 16-byte banks
    uint32_t sweep_frequency_ = 0u;       // channel 1's shadow frequency
    uint32_t sweep_timer_ = 0u;
    uint16_t noise_ = 0u;                 // channel 4 shift register
    bool noise_high_ = false;
    uint32_t sequencer_phase_ = 0u;       // cycles into the current 512 Hz step
    uint32_t sequencer_step_ = 0u;
    uint32_t psg_pending_ = 0u;           // cycles not yet applied to the PSG channels
    uint32_t sample_phase_ = 0u;
    // The output capacitor removes DC: the PSG's unipolar DACs leave an offset
    // (e.g. a 12.5% duty wave averages -3/4 of its volume) that would otherwise eat headroom.
    std::array<double, 2u> dc_input_{};
    std::array<double, 2u> dc_output_{};
    std::vector<int16_t> samples_;
};

} // namespace emugbds::ingba
