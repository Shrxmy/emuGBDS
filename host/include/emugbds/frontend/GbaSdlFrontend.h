#pragma once

#include "emugbds/cores/MGbaCoreAdapter.h"

#include <cstdint>
#include <string>

namespace emugbds::frontend {

bool RunGbaSdlFrontend(cores::MGbaCoreAdapter& core, int scale, uint64_t max_frames, double speed_multiplier, std::string& error);

} // namespace emugbds::frontend
