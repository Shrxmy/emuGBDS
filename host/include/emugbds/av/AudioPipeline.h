#pragma once

#include "emugbds/runtime/Types.h"

#include <cstdint>
#include <string>
#include <vector>

struct mCore;

#if EMUGBDS_HAS_MGBA
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#endif

namespace emugbds::av {

class AudioPipeline {
public:
    bool InitializeForGba(mCore* core, uint32_t output_sample_rate, uint32_t channels, std::string& error);
    bool PullGbaAudio(runtime::AudioChunk& out_chunk, std::string& error);
    void Reset() noexcept;

private:
    uint32_t output_sample_rate_ = 0;
    uint32_t channels_ = 0;
    uint32_t max_frames_per_tick_ = 0;

#if EMUGBDS_HAS_MGBA
    mCore* core_ = nullptr;
    mAudioBuffer mixed_audio_buffer_{};
    mAudioResampler audio_resampler_{};
    std::vector<int16_t> output_samples_;
    bool initialized_ = false;
#endif
};

} // namespace emugbds::av
