#pragma once

#include <chrono>
#include <string>

namespace emugbds::scheduler {

class FrameScheduler {
public:
    bool Configure(double target_fps, double speed_multiplier, std::string& error);
    void Start();
    void WaitForNextFrame();
    double effective_fps() const noexcept { return effective_fps_; }

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point frame_deadline_{};
    std::chrono::nanoseconds frame_interval_{0};
    bool started_ = false;
    double effective_fps_ = 0.0;
};

} // namespace emugbds::scheduler
