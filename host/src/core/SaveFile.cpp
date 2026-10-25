#include "emugbds/core/SaveFile.h"

#include <fstream>
#include <system_error>

namespace emugbds::core {

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
