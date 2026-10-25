#include "FileBrowser.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

namespace emugbds::frontend {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

void FileBrowser::Open(std::string title, const std::filesystem::path& directory, std::vector<std::string> extensions) {
    title_ = std::move(title);
    extensions_ = std::move(extensions);
    std::error_code error;
    const bool usable = !directory.empty() && std::filesystem::is_directory(directory, error);
    ChangeDirectory(usable ? directory : std::filesystem::current_path(error));
    open_requested_ = true;
}


void FileBrowser::ChangeDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(directory, error);
    if (error) {
        resolved = directory;
    }
    entries_.clear();
    list_error_.clear();
    selected_ = -1;
    directory_ = resolved;
    std::filesystem::directory_iterator it(resolved, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        list_error_ = "Cannot open folder: " + error.message();
    }
    for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
        std::error_code type_error;
        const bool folder = it->is_directory(type_error);
        const std::string extension = Lower(it->path().extension().u8string());
        if (!folder && !extensions_.empty() &&
            std::find(extensions_.begin(), extensions_.end(), extension) == extensions_.end()) {
            continue;
        }
        entries_.push_back({it->path().filename().u8string(), folder});
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) {
            return a.folder;
        }
        return Lower(a.name) < Lower(b.name);
    });
    const std::string text = resolved.u8string();
    std::strncpy(path_box_, text.c_str(), sizeof(path_box_) - 1u);
    path_box_[sizeof(path_box_) - 1u] = '\0';
}


bool FileBrowser::Draw(std::filesystem::path& chosen) {
    if (open_requested_) {
        ImGui::OpenPopup(title_.c_str());
        open_requested_ = false;
        open_ = true;
    }
    if (!open_) {
        return false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title_.c_str(), &open_)) {
        open_ = false;
        return false;
    }

    bool picked = false;
    std::filesystem::path next_directory;
    if (ImGui::Button("Up")) {
        next_directory = directory_.has_relative_path() ? directory_.parent_path() : directory_; // "D:\" stays put
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", path_box_, sizeof(path_box_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::filesystem::path typed = std::filesystem::u8path(path_box_);
        std::error_code error;
        if (std::filesystem::is_regular_file(typed, error)) {
            chosen = typed;
            picked = true;
        } else {
            next_directory = typed;
        }
    }
    if (!list_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", list_error_.c_str());
    }

    const float footer = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##entries", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const Entry& entry = entries_[static_cast<size_t>(i)];
            const std::string label = (entry.folder ? "[folder] " : "") + entry.name;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), selected_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_ = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (entry.folder) {
                        next_directory = directory_ / std::filesystem::u8path(entry.name);
                    } else {
                        chosen = directory_ / std::filesystem::u8path(entry.name);
                        picked = true;
                    }
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    const bool file_selected = selected_ >= 0 && !entries_[static_cast<size_t>(selected_)].folder;
    ImGui::BeginDisabled(!file_selected);
    if (ImGui::Button("Open") && file_selected) {
        chosen = directory_ / std::filesystem::u8path(entries_[static_cast<size_t>(selected_)].name);
        picked = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        open_ = false;
    }

    if (picked) {
        open_ = false;
    }
    if (!open_) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (!picked && !next_directory.empty()) {
        ChangeDirectory(next_directory);
    }
    return picked;
}

} // namespace emugbds::frontend
