#include "emugbds/scheduler/FrameScheduler.h"

#include <thread>

namespace emugbds::scheduler {

bool FrameScheduler::Configure(double target_fps, double speed_multiplier, std::string& error) {
    if (target_fps <= 0.0) {
        error = "FrameScheduler target FPS must be > 0";
        return false;
    }
    if (speed_multiplier <= 0.0) {
        error = "FrameScheduler speed multiplier must be > 0";
        return false;
    }

    effective_fps_ = target_fps * speed_multiplier;
    if (effective_fps_ < 1.0) {
        effective_fps_ = 1.0;
    }

    const double ns_per_frame = 1'000'000'000.0 / effective_fps_;
    frame_interval_ = std::chrono::nanoseconds(static_cast<int64_t>(ns_per_frame));
    if (frame_interval_.count() <= 0) {
        frame_interval_ = std::chrono::nanoseconds(1);
    }

    started_ = false;
    return true;
}

void FrameScheduler::Start() {
    frame_deadline_ = Clock::now();
    started_ = true;
}

void FrameScheduler::WaitForNextFrame() {
    if (!started_) {
        Start();
        return;
    }

    frame_deadline_ += frame_interval_;
    auto now = Clock::now();

    if (frame_deadline_ > now) {
        // Coarse sleep first, then a short spin to reduce oversleep jitter.
        const auto guard = std::chrono::microseconds(300);
        if (frame_deadline_ - now > guard) {
            std::this_thread::sleep_until(frame_deadline_ - guard);
        }
        while (Clock::now() < frame_deadline_) {
            // Busy wait for sub-millisecond precision.
        }
        return;
    }

    // If we're lagging too far behind, resync to current wall clock.
    if (now - frame_deadline_ > frame_interval_ * 4) {
        frame_deadline_ = now;
    }
}

} // namespace emugbds::scheduler
