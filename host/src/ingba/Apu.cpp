#include "emugbds/ingba/Apu.h"

#include <algorithm>

namespace emugbds::ingba {
namespace {

constexpr uint32_t kSequencerCycles = 32768u; // 16.78 MHz / 512 Hz

// Register offsets from 4000060h.
constexpr uint32_t kNr10 = 0x00u; // sweep
constexpr uint32_t kNr11 = 0x02u; // length, duty
constexpr uint32_t kNr12 = 0x03u; // envelope
constexpr uint32_t kNr13 = 0x04u; // frequency low
constexpr uint32_t kNr14 = 0x05u; // frequency high, length enable, trigger
constexpr uint32_t kNr21 = 0x08u;
constexpr uint32_t kNr22 = 0x09u;
constexpr uint32_t kNr23 = 0x0Cu;
constexpr uint32_t kNr24 = 0x0Du;
constexpr uint32_t kNr30 = 0x10u; // wave RAM dimension, bank, playback
constexpr uint32_t kNr31 = 0x12u;
constexpr uint32_t kNr32 = 0x13u; // volume (bits 5-6), force 75% (bit 7)
constexpr uint32_t kNr33 = 0x14u;
constexpr uint32_t kNr34 = 0x15u;
constexpr uint32_t kNr41 = 0x18u;
constexpr uint32_t kNr42 = 0x19u;
constexpr uint32_t kNr43 = 0x1Cu; // noise ratio, width, shift
constexpr uint32_t kNr44 = 0x1Du;

constexpr std::array<uint32_t, 4> kControl = {kNr14, kNr24, kNr34, kNr44};
constexpr std::array<uint32_t, 4> kEnvelope = {kNr12, kNr22, 0u, kNr42};
constexpr std::array<uint32_t, 4> kDutyHigh = {1u, 2u, 4u, 6u}; // 12.5%, 25%, 50%, 75% of 8 steps

} // namespace

void Apu::Reset() {
    fifos_ = {};
    psg_ = {};
    channels_ = {};
    wave_ram_ = {};
    sweep_frequency_ = 0u;
    sweep_timer_ = 0u;
    noise_ = 0u;
    noise_high_ = false;
    sequencer_phase_ = 0u;
    sequencer_step_ = 0u;
    psg_pending_ = 0u;
    sample_phase_ = 0u;
    dc_input_ = {};
    dc_output_ = {};
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


void Apu::WritePsg(uint32_t address, uint8_t value) {
    SyncPsg(); // the write takes effect from now
    const uint32_t offset = address - kPsgFirstAddress;
    psg_[offset] = value;
    switch (offset) {
    case kNr11: channels_[0].length = 64u - (value & 0x3Fu); break;
    case kNr21: channels_[1].length = 64u - (value & 0x3Fu); break;
    case kNr31: channels_[2].length = 256u - value; break;
    case kNr41: channels_[3].length = 64u - (value & 0x3Fu); break;
    case kNr12:
    case kNr22:
    case kNr42:
        if ((value & 0xF8u) == 0u) {
            channels_[offset == kNr12 ? 0u : offset == kNr22 ? 1u : 3u].on = false; // volume 0, decreasing: no sound
        }
        break;
    case kNr30:
        if ((value & 0x80u) == 0u) {
            channels_[2].on = false; // "Sound Channel 3 Off"
        }
        break;
    case kNr14:
    case kNr24:
    case kNr34:
    case kNr44:
        if ((value & 0x80u) != 0u) {
            Trigger(offset == kNr14 ? 0u : offset == kNr24 ? 1u : offset == kNr34 ? 2u : 3u);
        }
        break;
    default:
        break;
    }
}


void Apu::PowerOffPsg() {
    SyncPsg();
    psg_ = {};
    for (Channel& channel : channels_) {
        channel = Channel{};
    }
}


uint8_t Apu::active_channels() const noexcept {
    uint8_t flags = 0u;
    for (uint32_t i = 0u; i < channels_.size(); ++i) {
        flags = static_cast<uint8_t>(flags | (channels_[i].on ? 1u << i : 0u));
    }
    return flags;
}


uint8_t Apu::ReadWave(uint32_t index) const noexcept {
    const uint32_t bank = ((psg_[kNr30] >> 6u) & 1u) ^ 1u;
    return wave_ram_[bank * 16u + (index & 15u)];
}


void Apu::WriteWave(uint32_t index, uint8_t value) {
    SyncPsg();
    const uint32_t bank = ((psg_[kNr30] >> 6u) & 1u) ^ 1u;
    wave_ram_[bank * 16u + (index & 15u)] = value;
}


uint32_t Apu::Frequency(uint32_t channel) const noexcept {
    const uint32_t low = channel == 0u ? kNr13 : channel == 1u ? kNr23 : kNr33;
    return Reg(low) | ((Reg(low + 1u) & 7u) << 8u);
}


// Cycles per waveform step at 16.78 MHz (GBATEK frequencies).
int32_t Apu::Period(uint32_t channel) const noexcept {
    if (channel < 2u) {
        return static_cast<int32_t>(2048u - Frequency(channel)) * 16; // 131072/(2048-n) Hz over 8 duty steps
    }
    if (channel == 2u) {
        return static_cast<int32_t>(2048u - Frequency(channel)) * 8;  // 2097152/(2048-n) digits per second
    }
    // Noise: 524288 Hz / r / 2^(s+1), with r=0 counting as 0.5.
    const uint32_t ratio = Reg(kNr43) & 7u;
    const uint32_t shift = Reg(kNr43) >> 4u;
    return static_cast<int32_t>((ratio == 0u ? 16u : 32u * ratio) << (shift + 1u));
}


// The channel's current output, -15..+15 before mixing.
int32_t Apu::Level(uint32_t channel) const noexcept {
    const Channel& c = channels_[channel];
    if (!c.on) {
        return 0;
    }
    if (channel < 2u) {
        const uint32_t duty = Reg(channel == 0u ? kNr11 : kNr21) >> 6u;
        return c.position < kDutyHigh[duty] ? c.volume : -c.volume;
    }
    if (channel == 3u) {
        return noise_high_ ? c.volume : -c.volume;
    }
    // Wave: two 4-bit digits per byte, high nibble first; two-bank mode starts with the selected bank.
    const uint32_t selected = (Reg(kNr30) >> 6u) & 1u;
    const uint32_t bank = (Reg(kNr30) & 0x20u) != 0u ? (selected + c.position / 32u) & 1u : selected;
    const uint32_t digit = c.position % 32u;
    const uint8_t byte = wave_ram_[bank * 16u + digit / 2u];
    const int32_t sample = static_cast<int32_t>((digit & 1u) == 0u ? byte >> 4u : byte & 0x0Fu) * 2 - 15;
    if ((Reg(kNr32) & 0x80u) != 0u) {
        return sample * 3 / 4; // forced 75%
    }
    static constexpr std::array<int32_t, 4> kShift = {-1, 0, 1, 2}; // mute, 100%, 50%, 25%
    const int32_t shift = kShift[(Reg(kNr32) >> 5u) & 3u];
    return shift < 0 ? 0 : sample / (1 << shift);
}


void Apu::Trigger(uint32_t channel) {
    Channel& c = channels_[channel];
    c.on = true;
    if (c.length == 0u) {
        c.length = channel == 2u ? 256u : 64u;
    }
    c.timer = Period(channel);
    c.position = 0u;
    if (channel != 2u) {
        const uint8_t envelope = Reg(kEnvelope[channel]);
        c.volume = envelope >> 4u;
        c.envelope_timer = envelope & 7u;
        if ((envelope & 0xF8u) == 0u) {
            c.on = false; // no volume and no rise: the channel stays silent
        }
    } else if ((Reg(kNr30) & 0x80u) == 0u) {
        c.on = false;
    }
    if (channel == 0u) {
        sweep_frequency_ = Frequency(0u);
        sweep_timer_ = (Reg(kNr10) >> 4u) & 7u;
    } else if (channel == 3u) {
        noise_ = (Reg(kNr43) & 8u) != 0u ? 0x40u : 0x4000u; // GBATEK: X=40h (7-bit) or 4000h (15-bit)
        noise_high_ = false;
    }
}


void Apu::StepWaveform(uint32_t channel) {
    Channel& c = channels_[channel];
    if (channel < 2u) {
        c.position = (c.position + 1u) & 7u;
    } else if (channel == 2u) {
        c.position = (c.position + 1u) % ((Reg(kNr30) & 0x20u) != 0u ? 64u : 32u);
    } else {
        // GBATEK: X=X SHR 1, IF carry THEN Out=HIGH, X=X XOR 6000h (15-bit) / 60h (7-bit) ELSE Out=LOW.
        const bool carry = (noise_ & 1u) != 0u;
        noise_ = static_cast<uint16_t>(noise_ >> 1u);
        noise_high_ = carry;
        if (carry) {
            noise_ ^= (Reg(kNr43) & 8u) != 0u ? 0x60u : 0x6000u;
        }
    }
}


// 512 Hz: length at 256 Hz (even steps), sweep at 128 Hz (steps 2 and 6), envelope at 64 Hz (step 7).
void Apu::ClockSequencer() {
    sequencer_step_ = (sequencer_step_ + 1u) & 7u;
    if ((sequencer_step_ & 1u) == 0u) {
        for (uint32_t i = 0u; i < channels_.size(); ++i) {
            Channel& c = channels_[i];
            // The counter runs whenever length is enabled, so an expired one reloads on the next trigger.
            if ((Reg(kControl[i]) & 0x40u) != 0u && c.length > 0u && --c.length == 0u) {
                c.on = false;
            }
        }
    }
    if ((sequencer_step_ == 2u || sequencer_step_ == 6u) && channels_[0].on) {
        const uint32_t time = (Reg(kNr10) >> 4u) & 7u;
        if (time != 0u && (sweep_timer_ == 0u || --sweep_timer_ == 0u)) {
            sweep_timer_ = time;
            const uint32_t shift = Reg(kNr10) & 7u;
            const uint32_t delta = sweep_frequency_ >> shift;
            const uint32_t next = (Reg(kNr10) & 8u) != 0u ? sweep_frequency_ - delta : sweep_frequency_ + delta;
            if (next > 2047u) {
                channels_[0].on = false; // X(t) = X(t-1) +/- X(t-1)/2^n overflowed
            } else if (shift != 0u) {
                sweep_frequency_ = next;
                psg_[kNr13] = static_cast<uint8_t>(next);
                psg_[kNr14] = static_cast<uint8_t>((psg_[kNr14] & ~7u) | (next >> 8u));
            }
        }
    }
    if (sequencer_step_ == 7u) {
        for (uint32_t i : {0u, 1u, 3u}) {
            Channel& c = channels_[i];
            const uint8_t envelope = Reg(kEnvelope[i]);
            const uint32_t step_time = envelope & 7u;
            if (!c.on || step_time == 0u) {
                continue;
            }
            if (c.envelope_timer > 1u) {
                --c.envelope_timer;
                continue;
            }
            c.envelope_timer = step_time;
            if ((envelope & 8u) != 0u) {
                c.volume = std::min(c.volume + 1, 15);
            } else {
                c.volume = std::max(c.volume - 1, 0);
            }
        }
    }
}


void Apu::SyncPsg() {
    RunPsg(psg_pending_);
    psg_pending_ = 0u;
}


void Apu::RunPsg(uint32_t cycles) {
    while (cycles > 0u) {
        const uint32_t step = std::min(cycles, kSequencerCycles - sequencer_phase_);
        for (uint32_t i = 0u; i < channels_.size(); ++i) {
            Channel& c = channels_[i];
            int32_t remaining = static_cast<int32_t>(step);
            while (c.on && remaining > 0) {
                const int32_t chunk = std::min(remaining, c.timer);
                c.accumulated += Level(i) * chunk;
                c.timer -= chunk;
                remaining -= chunk;
                if (c.timer <= 0) {
                    StepWaveform(i);
                    c.timer = Period(i);
                }
            }
        }
        sequencer_phase_ += step;
        cycles -= step;
        if (sequencer_phase_ == kSequencerCycles) {
            sequencer_phase_ = 0u;
            ClockSequencer();
        }
    }
}


void Apu::Advance(uint32_t cycles, uint16_t soundcnt_l, uint16_t soundcnt_h, uint16_t soundcnt_x) {
    sample_phase_ += cycles;
    psg_pending_ += cycles;
    while (sample_phase_ >= kCyclesPerSample) {
        sample_phase_ -= kCyclesPerSample;
        // The sample point was sample_phase_ cycles ago: catch the PSG up to it, then mix.
        RunPsg(psg_pending_ - sample_phase_);
        psg_pending_ = sample_phase_;
        Mix(soundcnt_l, soundcnt_h, soundcnt_x);
    }
}


void Apu::Mix(uint16_t soundcnt_l, uint16_t soundcnt_h, uint16_t soundcnt_x) {
    int32_t left = 0;
    int32_t right = 0;
    if ((soundcnt_x & 0x0080u) != 0u) {
        // PSG: each channel spans +/-15 x master volume (1-8), then SOUNDCNT_H's 25/50/100%,
        // so one channel at full settings reaches about a quarter of the output range (GBATEK).
        int32_t psg_left = 0;
        int32_t psg_right = 0;
        for (uint32_t i = 0u; i < channels_.size(); ++i) {
            const int32_t level = channels_[i].accumulated / static_cast<int32_t>(kCyclesPerSample);
            if (((soundcnt_l >> (12u + i)) & 1u) != 0u) {
                psg_left += level;
            }
            if (((soundcnt_l >> (8u + i)) & 1u) != 0u) {
                psg_right += level;
            }
        }
        const uint32_t psg_volume = std::min<uint32_t>(soundcnt_h & 3u, 2u);
        const std::array<int32_t, 2> psg = {
            (psg_left * static_cast<int32_t>(((soundcnt_l >> 4u) & 7u) + 1u)) >> (2u - psg_volume),
            (psg_right * static_cast<int32_t>((soundcnt_l & 7u) + 1u)) >> (2u - psg_volume)};
        // shortcut: a one-pole DC blocker (about 5 Hz) stands in for the output capacitor.
        constexpr double kPole = 0.999;
        for (size_t side = 0u; side < 2u; ++side) {
            dc_output_[side] = psg[side] - dc_input_[side] + kPole * dc_output_[side];
            dc_input_[side] = psg[side];
        }
        left += static_cast<int32_t>(dc_output_[0]);
        right += static_cast<int32_t>(dc_output_[1]);

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
    for (Channel& channel : channels_) {
        channel.accumulated = 0;
    }
    // shortcut: assumes the default SOUNDBIAS of 0x200, so the 10-bit output clips at -0x200..0x1FF.
    const auto to_pcm = [](int32_t level) {
        return static_cast<int16_t>(std::clamp(level, -0x200, 0x1FF) * 64);
    };
    samples_.push_back(to_pcm(left));
    samples_.push_back(to_pcm(right));
}

} // namespace emugbds::ingba
