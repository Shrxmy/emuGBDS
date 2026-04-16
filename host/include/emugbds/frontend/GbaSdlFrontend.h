#pragma once

#include "emugbds/runtime/RuntimeKernel.h"

#include <cstdint>
#include <string>

namespace emugbds::frontend {

bool RunGbaSdlFrontend(runtime::RuntimeKernel& kernel, int scale, uint64_t max_frames, double speed_multiplier, std::string& error);

} // namespace emugbds::frontend
