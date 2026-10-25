#pragma once

// File helpers shared by both cores.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::core {

// Reads a whole file into one exactly-sized buffer (no growth while reading, so a 64 MiB
// ROM costs 64 MiB), padded with zeros to a multiple of `round_to` bytes. Empty files fail.
bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes, std::string& error,
                   size_t round_to = 1u);

// Writes save data without risking the old file: the bytes go to a temporary
// file next to `path`, which is flushed, closed, and then renamed over it.
bool WriteFileReplacing(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& error);

} // namespace emugbds::core
