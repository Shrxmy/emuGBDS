#include "emugbds/core/SaveFile.h"

#include <fstream>
#include <system_error>

namespace emugbds::core {

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes, std::string& error,
                   size_t round_to) {
    std::error_code size_error;
    const uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        error = "Failed to open file: " + path.string();
        return false;
    }
    if (size == 0u) {
        error = "File is empty: " + path.string();
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open file: " + path.string();
        return false;
    }
    std::vector<uint8_t> bytes((static_cast<size_t>(size) + round_to - 1u) / round_to * round_to, 0u);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        error = "Failed to read file: " + path.string();
        return false;
    }
    out_bytes = std::move(bytes); // the old contents are freed here, not kept alongside
    return true;
}


bool WriteFileReplacing(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        output.close();
        if (output.fail()) {
            error = "Failed to write save file: " + temporary.string();
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        error = "Failed to replace save file " + path.string() + ": " + rename_error.message();
        return false;
    }
    return true;
}

} // namespace emugbds::core
