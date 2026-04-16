#pragma once

#include "emugbds/runtime/Types.h"

#include <string>
#include <vector>

struct mCore;

#if EMUGBDS_HAS_MGBA
#include <mgba-util/image.h>
#endif

namespace emugbds::av {

class VideoPipeline {
public:
    bool InitializeForGba(mCore* core, std::string& error);
    bool CaptureGbaFrame(runtime::VideoFrame& out_frame, std::string& error);
    void Reset() noexcept;

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;

#if EMUGBDS_HAS_MGBA
    std::vector<mColor> frame_buffer_;
    std::vector<uint32_t> upload_buffer_;
    mCore* core_ = nullptr;
#endif
};

} // namespace emugbds::av
