#include "emugbds/av/AudioPipeline.h"

#if EMUGBDS_HAS_MGBA
#include <mgba/core/core.h>
#endif

namespace emugbds::av {

bool AudioPipeline::InitializeForGba(mCore* core, uint32_t output_sample_rate, uint32_t channels, std::string& error) {
#if !EMUGBDS_HAS_MGBA
    (void)core;
    (void)output_sample_rate;
    (void)channels;
    error = "Audio pipeline is unavailable because mGBA support is disabled";
    return false;
#else
    if (!core) {
        error = "Audio pipeline requires a valid mGBA core";
        return false;
    }
    if (output_sample_rate == 0 || channels == 0) {
        error = "Audio pipeline requires non-zero output sample rate and channels";
        return false;
    }

    Reset();

    core_ = core;
    output_sample_rate_ = output_sample_rate;
    channels_ = channels;
    max_frames_per_tick_ = output_sample_rate_ / 20;
    if (max_frames_per_tick_ < 1) {
        max_frames_per_tick_ = 1;
    }

    core_->setAudioBufferSize(core_, 2048);
    mAudioBufferInit(&mixed_audio_buffer_, static_cast<size_t>(output_sample_rate_) / 5, static_cast<int>(channels_));
    mAudioResamplerInit(&audio_resampler_, mINTERPOLATOR_SINC);
    mAudioResamplerSetDestination(&audio_resampler_, &mixed_audio_buffer_, output_sample_rate_);
    initialized_ = true;
    return true;
#endif
}

bool AudioPipeline::PullGbaAudio(runtime::AudioChunk& out_chunk, std::string& error) {
#if !EMUGBDS_HAS_MGBA
    (void)out_chunk;
    error = "Audio pipeline is unavailable because mGBA support is disabled";
    return false;
#else
    if (!initialized_ || !core_) {
        error = "Audio pipeline is not initialized";
        return false;
    }

    out_chunk = {};

    mAudioBuffer* audio_buffer = core_->getAudioBuffer(core_);
    if (!audio_buffer) {
        return true;
    }

    unsigned source_rate = core_->audioSampleRate(core_);
    if (source_rate == 0) {
        source_rate = 32768;
    }
    mAudioResamplerSetSource(&audio_resampler_, audio_buffer, source_rate, true);
    mAudioResamplerProcess(&audio_resampler_);

    const size_t available_frames = mAudioBufferAvailable(&mixed_audio_buffer_);
    if (available_frames == 0) {
        return true;
    }

    const size_t frames_to_read = available_frames < max_frames_per_tick_
        ? available_frames
        : max_frames_per_tick_;
    output_samples_.resize(frames_to_read * channels_);
    const size_t read_frames = mAudioBufferRead(&mixed_audio_buffer_, output_samples_.data(), frames_to_read);
    if (read_frames == 0) {
        return true;
    }

    out_chunk.sample_rate = output_sample_rate_;
    out_chunk.channels = channels_;
    out_chunk.samples = output_samples_.data();
    out_chunk.sample_count = read_frames * channels_;
    return true;
#endif
}

void AudioPipeline::Reset() noexcept {
    output_sample_rate_ = 0;
    channels_ = 0;
    max_frames_per_tick_ = 0;
#if EMUGBDS_HAS_MGBA
    if (initialized_) {
        mAudioResamplerDeinit(&audio_resampler_);
        mAudioBufferDeinit(&mixed_audio_buffer_);
    }
    output_samples_.clear();
    initialized_ = false;
    core_ = nullptr;
#endif
}

} // namespace emugbds::av
