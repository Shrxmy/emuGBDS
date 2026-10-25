#pragma once

#include "emugbds/runtime/Types.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace emugbds::boot {

// The default BIOS/firmware files live under bios/ in the working directory, or in
// one of `extra_roots` (checked in order after it), e.g. the executable's folder.
std::optional<std::filesystem::path> DetectDefaultGbaBios(const std::vector<std::filesystem::path>& extra_roots = {});
std::optional<std::filesystem::path> DetectDefaultNdsBios7(const std::vector<std::filesystem::path>& extra_roots = {});
std::optional<std::filesystem::path> DetectDefaultNdsBios9(const std::vector<std::filesystem::path>& extra_roots = {});
std::optional<std::filesystem::path> DetectDefaultNdsFirmware(const std::vector<std::filesystem::path>& extra_roots = {});
void ResolveDefaultBootPaths(runtime::BootConfig& config, const std::vector<std::filesystem::path>& extra_roots = {});

} // namespace emugbds::boot
