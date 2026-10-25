#pragma once

// A small Dear ImGui file picker built on std::filesystem (SDL2 has no native
// file dialog): a path box, an Up button, and a list of folders and matching files.

#include <filesystem>
#include <string>
#include <vector>

namespace emugbds::frontend {

class FileBrowser {
public:
    // Shows the browser at `directory` (or the working directory) listing files whose
    // lower-case extension is in `extensions` (empty = every file).
    void Open(std::string title, const std::filesystem::path& directory, std::vector<std::string> extensions);
    // Draws the browser while it is open; returns true once, with the picked file in `chosen`.
    bool Draw(std::filesystem::path& chosen);
    const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    struct Entry {
        std::string name; // UTF-8
        bool folder = false;
    };

    void ChangeDirectory(const std::filesystem::path& directory);

    std::string title_;
    std::vector<std::string> extensions_;
    std::filesystem::path directory_;
    std::vector<Entry> entries_;
    std::string list_error_;
    char path_box_[1024] = {};
    int selected_ = -1;
    bool open_ = false;
    bool open_requested_ = false;
};

} // namespace emugbds::frontend
