#pragma once

// Writes save data without risking the old file: the bytes go to a temporary
// file next to `path`, which is flushed, closed, and then renamed over it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::core {

bool WriteFileReplacing(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& error);

} // namespace emugbds::core
