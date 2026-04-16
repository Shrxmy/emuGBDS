#pragma once

#include "emugbds/runtime/Types.h"

#include <filesystem>
#include <optional>

namespace emugbds::boot {

std::optional<std::filesystem::path> DetectDefaultGbaBios();
std::optional<std::filesystem::path> DetectDefaultNdsBios7();
std::optional<std::filesystem::path> DetectDefaultNdsBios9();
std::optional<std::filesystem::path> DetectDefaultNdsFirmware();
void ResolveDefaultBootPaths(runtime::BootConfig& config);

} // namespace emugbds::boot
