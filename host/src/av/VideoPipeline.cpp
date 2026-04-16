#include "emugbds/av/VideoPipeline.h"

#if EMUGBDS_HAS_MGBA
#include <mgba/core/core.h>
#include <mgba-util/image.h>
#endif

namespace emugbds::av {

bool VideoPipeline::InitializeForGba(mCore* core, std::string& error) {
#if !EMUGBDS_HAS_MGBA
    (void)core;
    error = "Video pipeline is unavailable because mGBA support is disabled";
    return false;
#else
    if (!core) {
        error = "Video pipeline requires a valid mGBA core";
        return false;
    }

    unsigned width = 0;
    unsigned height = 0;
    core->baseVideoSize(core, &width, &height);
    if (width == 0 || height == 0) {
        error = "mGBA core reported invalid video dimensions";
        return false;
    }

    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
    stride_ = width_ < 256 ? 256 : width_;
    frame_buffer_.assign(static_cast<size_t>(stride_) * height_, 0);
    upload_buffer_.assign(width_ * height_, 0);
    core_ = core;

    core_->setVideoBuffer(core_, frame_buffer_.data(), stride_);
    return true;
#endif
}

bool VideoPipeline::CaptureGbaFrame(runtime::VideoFrame& out_frame, std::string& error) {
#if !EMUGBDS_HAS_MGBA
    (void)out_frame;
    error = "Video pipeline is unavailable because mGBA support is disabled";
    return false;
#else
    if (!core_) {
        error = "Video pipeline is not initialized";
        return false;
    }

    for (uint32_t y = 0; y < height_; ++y) {
        const size_t src_row = static_cast<size_t>(y) * stride_;
        const size_t dst_row = static_cast<size_t>(y) * width_;
        for (uint32_t x = 0; x < width_; ++x) {
            const size_t src_index = src_row + x;
            const size_t dst_index = dst_row + x;
#if !defined(COLOR_16_BIT)
            const uint32_t converted = static_cast<uint32_t>(frame_buffer_[src_index]) | 0xFF000000u;
#else
            const uint32_t converted = mColorConvert(
                static_cast<uint32_t>(frame_buffer_[src_index]), mCOLOR_NATIVE, mCOLOR_ABGR8);
#endif
            upload_buffer_[dst_index] = converted;
        }
    }

    out_frame.format = runtime::PixelFormat::ABGR8888;
    out_frame.width = width_;
    out_frame.height = height_;
    out_frame.pixels = upload_buffer_.data();
    return true;
#endif
}

void VideoPipeline::Reset() noexcept {
    width_ = 0;
    height_ = 0;
    stride_ = 0;
#if EMUGBDS_HAS_MGBA
    frame_buffer_.clear();
    upload_buffer_.clear();
    core_ = nullptr;
#endif
}

} // namespace emugbds::av
