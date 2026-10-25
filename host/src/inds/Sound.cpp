#include "emugbds/inds/Sound.h"

#include "emugbds/inds/MemoryBus.h"

#include <algorithm>

namespace emugbds::inds {
namespace {

constexpr uint32_t kStart = 1u << 31;
constexpr uint32_t kHold = 1u << 15;
constexpr uint32_t kControlMask = 0xFF7F837Fu; // SOUNDxCNT bits that exist
constexpr uint32_t kAddressMask = 0x07FFFFFCu;
constexpr uint32_t kFormatPsg = 3u;
constexpr uint32_t kFormatAdpcm = 2u;
constexpr uint32_t kRepeatOneShot = 2u;
constexpr uint32_t kSoundCnt = 0x100u;
constexpr uint32_t kSoundBias = 0x104u;
constexpr uint32_t kCaptureCnt = 0x108u;
constexpr uint32_t kCaptureDestination = 0x110u; // capture 1 at +8
constexpr uint32_t kCaptureLength = 0x114u;
constexpr uint8_t kCaptureBusy = 0x80u;

constexpr std::array<int32_t, 4> kDivShift = {0, 1, 2, 4};
constexpr std::array<int32_t, 8> kAdpcmIndexStep = {-1, -1, -1, -1, 2, 4, 6, 8};
constexpr std::array<int32_t, 89> kAdpcmTable = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF};

uint32_t Merge(uint32_t old_value, uint32_t value, uint32_t mask) noexcept {
    return (old_value & ~mask) | (value & mask);
}

// 7-bit volume/panning factors: register values 0-126 mean N/128, 127 means 128/128.
int64_t Scale(uint32_t value) noexcept {
    return value == 127u ? 128 : static_cast<int64_t>(value);
}

} // namespace

void Sound::Reset() {
    channels_ = {};
    captures_ = {};
    control_ = 0u;
    bias_ = 0u;
    pending_ = 0u;
    phase_ = 0u;
    samples_.clear();
}


uint32_t Sound::ReadWord(uint32_t offset) {
    Sync(); // busy flags are current
    if (offset < kSoundCnt) {
        // Only SOUNDxCNT reads back; SAD, TMR, PNT, and LEN are write-only.
        return (offset & 0xFu) == 0u ? channels_[offset >> 4u].control : 0u;
    }
    switch (offset) {
    case kSoundCnt: return control_;
    case kSoundBias: return bias_;
    case kCaptureCnt: return captures_[0].control | (static_cast<uint32_t>(captures_[1].control) << 8u);
    case kCaptureDestination: return captures_[0].destination;
    case kCaptureDestination + 8u: return captures_[1].destination;
    default: return 0u; // capture lengths are write-only
    }
}


void Sound::WriteWord(uint32_t offset, uint32_t value, uint32_t mask) {
    Sync(); // the change takes effect from now, not from the last output sample
    if (offset < kSoundCnt) {
        const uint32_t index = offset >> 4u;
        Channel& channel = channels_[index];
        switch (offset & 0xFu) {
        case 0x0u: {
            const bool was_busy = (channel.control & kStart) != 0u;
            channel.control = Merge(channel.control, value, mask) & kControlMask;
            if ((mask & kStart) != 0u) {
                if ((value & kStart) == 0u) {
                    channel.active = false; // stopped by software
                    channel.ending = false;
                    channel.sample = 0;
                } else if (!was_busy) {
                    Start(index); // a 0 -> 1 change of the start bit (re)starts the sound
                }
            }
            if (!channel.active && (channel.control & kHold) == 0u) {
                channel.sample = 0; // clearing Hold releases a held one-shot sample
            }
            break;
        }
        case 0x4u:
            channel.source = Merge(channel.source, value, mask) & kAddressMask;
            break;
        case 0x8u: {
            const uint32_t word = Merge(channel.timer | (static_cast<uint32_t>(channel.loop_start) << 16u), value, mask);
            channel.timer = static_cast<uint16_t>(word);
            channel.loop_start = static_cast<uint16_t>(word >> 16u);
            break;
        }
        default:
            channel.length = Merge(channel.length, value, mask) & 0x003FFFFFu;
            break;
        }
        return;
    }

    switch (offset) {
    case kSoundCnt:
        control_ = static_cast<uint16_t>(Merge(control_, value, mask) & 0xBF7Fu);
        break;
    case kSoundBias:
        bias_ = static_cast<uint16_t>(Merge(bias_, value, mask) & 0x3FFu);
        break;
    case kCaptureCnt:
        for (uint32_t x = 0u; x < 2u; ++x) {
            if (((mask >> (8u * x)) & 0xFFu) == 0u) {
                continue;
            }
            Capture& capture = captures_[x];
            const bool was_busy = (capture.control & kCaptureBusy) != 0u;
            capture.control = static_cast<uint8_t>((value >> (8u * x)) & 0x8Fu);
            if ((capture.control & kCaptureBusy) == 0u) {
                capture.active = false;
            } else if (!was_busy) {
                capture.active = true;
                capture.position = 0u;
                capture.remaining = Period(channels_[x * 2u + 1u].timer); // shares channel 1/3's timer
            }
        }
        break;
    case kCaptureDestination:
    case kCaptureDestination + 8u: {
        Capture& capture = captures_[(offset - kCaptureDestination) / 8u];
        capture.destination = Merge(capture.destination, value, mask) & kAddressMask;
        break;
    }
    case kCaptureLength:
    case kCaptureLength + 8u: {
        Capture& capture = captures_[(offset - kCaptureLength) / 8u];
        capture.length = static_cast<uint16_t>(Merge(capture.length, value, mask));
        break;
    }
    default:
        break;
    }
}


void Sound::Advance(uint32_t cycles) {
    pending_ += cycles;
    phase_ += static_cast<uint64_t>(cycles) * kSampleRate;
    while (phase_ >= kArm7Clock) {
        phase_ -= kArm7Clock;
        // The sample point was `overshoot` cycles ago: run the channels up to it, then mix.
        const uint32_t overshoot = static_cast<uint32_t>(phase_ / kSampleRate);
        Run(pending_ - overshoot);
        pending_ = overshoot;
        Mix();
    }
}


void Sound::Sync() {
    Run(pending_);
    pending_ = 0u;
}


void Sound::Run(uint32_t cycles) {
    if (!captures_[0].active && !captures_[1].active) {
        RunChannels(cycles);
        return;
    }
    // Captures record the live output, so the channels run up to each capture event first.
    while (cycles > 0u) {
        uint32_t step = cycles;
        for (const Capture& capture : captures_) {
            if (capture.active) {
                step = std::min(step, static_cast<uint32_t>(std::max(capture.remaining, 1)));
            }
        }
        RunChannels(step);
        for (uint32_t x = 0u; x < captures_.size(); ++x) {
            Capture& capture = captures_[x];
            if (!capture.active) {
                continue;
            }
            capture.remaining -= static_cast<int32_t>(step);
            while (capture.active && capture.remaining <= 0) {
                StepCapture(x);
                capture.remaining += Period(channels_[x * 2u + 1u].timer);
            }
        }
        cycles -= step;
    }
}


void Sound::RunChannels(uint32_t cycles) {
    if (cycles == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < channels_.size(); ++i) {
        Channel& channel = channels_[i];
        if (!channel.active) {
            continue;
        }
        channel.remaining -= static_cast<int32_t>(cycles);
        while (channel.active && channel.remaining <= 0) {
            Step(i);
            channel.remaining += Period(channel.timer);
        }
    }
}


void Sound::Start(uint32_t index) {
    Channel& channel = channels_[index];
    const uint32_t format = (channel.control >> 29u) & 3u;
    channel.active = true;
    channel.ending = false;
    channel.sample = 0;
    channel.position = 0u;
    channel.remaining = Period(channel.timer);
    if (format == kFormatPsg) {
        channel.stalled = false;
        channel.delay = 1u; // GBATEK "Sound delayed Start": PSG/noise 1 sample, PCM 3, ADPCM 11
        channel.noise = 0x7FFFu;
        return;
    }
    // shortcut: PNT+LEN under 4 words hangs with busy set (GBATEK); nothing more is modelled.
    channel.stalled = static_cast<uint32_t>(channel.loop_start) + channel.length < 4u;
    channel.delay = format == kFormatAdpcm ? 11u : 3u;
    if (format == kFormatAdpcm) {
        const uint32_t header = memory_.DmaRead(false, channel.source, 4u);
        channel.adpcm = static_cast<int16_t>(header & 0xFFFFu);
        channel.adpcm_index = std::min<int32_t>(static_cast<int32_t>((header >> 16u) & 0x7Fu), 88);
        channel.loop_adpcm = channel.adpcm;
        channel.loop_adpcm_index = channel.adpcm_index;
        channel.position = 8u; // nibbles: the data follows the 32-bit header
    }
}


// One timer overflow: the channel moves to its next sample.
void Sound::Step(uint32_t index) {
    Channel& channel = channels_[index];
    if (channel.ending) {
        channel.active = false;
        channel.ending = false;
        if ((channel.control & kHold) == 0u) {
            channel.sample = 0;
        }
        return;
    }
    if (channel.stalled) {
        return;
    }
    if (channel.delay > 0u && --channel.delay > 0u) {
        return; // the overflow that ends the delay plays the first sample
    }

    const uint32_t format = (channel.control >> 29u) & 3u;
    if (format == kFormatPsg) {
        if (index >= 14u) {
            // Noise: X = X SHR 1, IF carry THEN Out=LOW, X = X XOR 6000h ELSE Out=HIGH.
            const bool carry = (channel.noise & 1u) != 0u;
            channel.noise = static_cast<uint16_t>(channel.noise >> 1u);
            if (carry) {
                channel.noise ^= 0x6000u;
            }
            channel.sample = carry ? -0x7FFF : 0x7FFF;
        } else if (index >= 8u) {
            // Duty N: the last N+1 of every 8 steps are HIGH; N=7 is always LOW.
            const uint32_t duty = (channel.control >> 24u) & 7u;
            channel.sample = duty != 7u && channel.position >= 7u - duty ? 0x7FFF : -0x7FFF;
            channel.position = (channel.position + 1u) & 7u;
        } else {
            channel.sample = 0; // channels 0-7 have no PSG
        }
        return;
    }

    const uint32_t unit = format == kFormatAdpcm ? 8u : 4u; // positions per word
    if (format == 0u) {
        channel.sample = static_cast<int8_t>(memory_.DmaRead(false, channel.source + channel.position, 1u)) * 256;
        channel.position += 1u;
    } else if (format == 1u) {
        channel.sample = static_cast<int16_t>(memory_.DmaRead(false, channel.source + channel.position, 2u));
        channel.position += 2u;
    } else {
        FetchAdpcm(channel);
    }
    if (channel.position >= (static_cast<uint32_t>(channel.loop_start) + channel.length) * unit) {
        if (((channel.control >> 27u) & 3u) >= kRepeatOneShot) {
            // Busy clears at the start of the last sample period; the sample still plays out.
            channel.control &= ~kStart;
            channel.ending = true;
        } else {
            channel.position = static_cast<uint32_t>(channel.loop_start) * unit;
            if (format == kFormatAdpcm) {
                channel.adpcm = channel.loop_adpcm;
                channel.adpcm_index = channel.loop_adpcm_index;
            }
        }
    }
}


void Sound::FetchAdpcm(Channel& channel) {
    if (channel.position == static_cast<uint32_t>(channel.loop_start) * 8u) {
        channel.loop_adpcm = channel.adpcm; // the loop restarts with the state seen here
        channel.loop_adpcm_index = channel.adpcm_index;
    }
    const uint32_t byte = memory_.DmaRead(false, channel.source + channel.position / 2u, 1u);
    const uint32_t data = (channel.position & 1u) != 0u ? byte >> 4u : byte & 0xFu;
    const int32_t step = kAdpcmTable[static_cast<size_t>(channel.adpcm_index)];
    // GBATEK's hardware rounding: the difference is built from step/8, step/4, step/2 and step.
    int32_t diff = step / 8;
    if ((data & 1u) != 0u) {
        diff += step / 4;
    }
    if ((data & 2u) != 0u) {
        diff += step / 2;
    }
    if ((data & 4u) != 0u) {
        diff += step;
    }
    channel.adpcm = (data & 8u) != 0u ? std::max(channel.adpcm - diff, -0x7FFF) : std::min(channel.adpcm + diff, 0x7FFF);
    channel.adpcm_index = std::clamp(channel.adpcm_index + kAdpcmIndexStep[data & 7u], 0, 88);
    channel.sample = channel.adpcm;
    channel.position += 1u;
}


// A channel after its volume divider and factor (16.11), GBATEK "Channel/Mixer Bit-Widths".
int64_t Sound::Volume(uint32_t index) const noexcept {
    const Channel& channel = channels_[index];
    const int64_t value = (static_cast<int64_t>(channel.sample) * 16) >> kDivShift[(channel.control >> 8u) & 3u]; // 16.4
    return value * Scale(channel.control & 0x7Fu);
}


// Left/right mixer sums (20.8); `routed` gets ch1 left/right and ch3 left/right (16.8).
void Sound::Mixers(int64_t& left_sum, int64_t& right_sum, std::array<int64_t, 4>& routed) const noexcept {
    left_sum = 0;
    right_sum = 0;
    for (uint32_t i = 0u; i < channels_.size(); ++i) {
        if (channels_[i].sample == 0) {
            if (i == 1u || i == 3u) {
                routed[i - 1u] = 0;
                routed[i] = 0;
            }
            continue;
        }
        const int64_t value = Volume(i);
        const int64_t pan = Scale((channels_[i].control >> 16u) & 0x7Fu);
        const int64_t left = (value * (128 - pan)) >> 10; // 16.18 -> 16.8
        const int64_t right = (value * pan) >> 10;
        if (i == 1u || i == 3u) {
            routed[i - 1u] = left;
            routed[i] = right;
            if ((control_ & (i == 1u ? 0x1000u : 0x2000u)) != 0u) {
                continue; // "Output Ch1/Ch3 to Mixer" off
            }
        }
        left_sum += left;
        right_sum += right;
    }
}


// One output sample: mixer, SOUNDCNT output select, master volume, bias, and the 10-bit clip.
void Sound::Mix() {
    int64_t mixer_left = 0;
    int64_t mixer_right = 0;
    std::array<int64_t, 4> routed{};
    Mixers(mixer_left, mixer_right, routed);
    const auto output = [&](uint32_t select, int64_t mixer, int64_t ch1, int64_t ch3) -> int16_t {
        const int64_t source = select == 0u ? mixer : select == 1u ? ch1 : select == 2u ? ch3 : ch1 + ch3;
        // Master volume N/128/64 and the 8 fraction bits: 20.8 -> 14.0 (the 10-bit DAC range).
        const int64_t level = (control_ & 0x8000u) != 0u ? (source * Scale(control_ & 0x7Fu)) >> 21 : 0;
        const int64_t speaker = std::clamp<int64_t>(level + bias_, 0, 0x3FF);
        // shortcut: the speakers' AC coupling is modelled by removing the programmed bias, so a bias
        // change makes no pop (the BIOS ramps it to avoid one anyway).
        return static_cast<int16_t>(std::clamp<int64_t>((speaker - bias_) * 64, -32768, 32767));
    };
    samples_.push_back(output((control_ >> 8u) & 3u, mixer_left, routed[0], routed[2]));
    samples_.push_back(output((control_ >> 10u) & 3u, mixer_right, routed[1], routed[3]));
}


void Sound::StepCapture(uint32_t index) {
    Capture& capture = captures_[index];
    // Source, sampled now: channel 0/2 after its volume, or the left/right mixer clipped to 16 bits.
    int32_t value = 0;
    if ((capture.control & 2u) != 0u) {
        value = static_cast<int32_t>(Volume(index * 2u) >> 11);
    } else {
        int64_t left = 0;
        int64_t right = 0;
        std::array<int64_t, 4> routed{};
        Mixers(left, right, routed);
        value = static_cast<int32_t>(std::clamp<int64_t>(index == 0u ? left : right, -0x800000, 0x7FFFFF) >> 8);
    }
    const uint32_t address = capture.destination + capture.position;
    if ((capture.control & 8u) != 0u) {
        memory_.DmaWrite(false, address, static_cast<uint8_t>(value >> 8), 1u); // PCM8
        capture.position += 1u;
    } else {
        memory_.DmaWrite(false, address, static_cast<uint16_t>(value), 2u);
        capture.position += 2u;
    }
    if (capture.position >= std::max<uint32_t>(capture.length, 1u) * 4u) {
        if ((capture.control & 4u) != 0u) {
            capture.active = false; // one-shot
            capture.control &= static_cast<uint8_t>(~kCaptureBusy);
        } else {
            capture.position = 0u;
        }
    }
}

} // namespace emugbds::inds
