#include "package_browser.h"

#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "localization/ui_text.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace dcp {
namespace {

constexpr char kPackageTitle[] = "Select your Dragon's Crown Pro package (.pkg)";
constexpr char kFolderTitle[] = "Select your Dragon's Crown Pro extracted game folder";
constexpr char kExtension[] = ".pkg";

fs::path HomePlace(const std::vector<Place>& places) {
    for (const Place& place : places) {
        if (place.label == "Home") {
            return place.path;
        }
    }
    return places.empty() ? fs::current_path() : places.front().path;
}

} // namespace

void PackageBrowser::Open(const fs::path& program_folder, InstallSource source) {
    places_ = StandardPlaces(program_folder);
    std::error_code ec;
    source_ = source;
    show_all_ = false;
    active_ = true;
    popup_open_ = false;
    const bool folders = source == InstallSource::kFolder;
    Go(!folder_.empty() && fs::is_directory(folder_, ec) ? folder_
       : folders                                        ? HomePlace(places_)
                                                        : StartingFolder(places_, kExtension));
}

void PackageBrowser::Go(const fs::path& folder) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(folder, ec);
    folder_ = ec ? folder : resolved;
    const bool every_file = show_all_ || source_ == InstallSource::kFolder;
    listing_ = ListFolder(folder_, every_file ? std::string_view() : std::string_view(kExtension), false);
    focus_list_ = true;
}

std::optional<fs::path> PackageBrowser::Draw(ImFont* font, float font_size, const trg::Theme& theme) {
    if (!active_) {
        return std::nullopt;
    }
    const bool folders = source_ == InstallSource::kFolder;
    const char* title = Tr(folders ? kFolderTitle : kPackageTitle);
    if (!popup_open_) {
        ImGui::OpenPopup(title);
        popup_open_ = true;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.88f, viewport->Size.y * 0.86f), ImGuiCond_Always);
    bool keep = true;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushFont(font, font_size);
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        {ImGuiCol_WindowBg, theme.popup},          {ImGuiCol_PopupBg, theme.popup},
        {ImGuiCol_TitleBg, theme.panel},           {ImGuiCol_TitleBgActive, theme.panel},
        {ImGuiCol_ChildBg, theme.panel},           {ImGuiCol_FrameBg, theme.frame},
        {ImGuiCol_FrameBgHovered, theme.frame_hot}, {ImGuiCol_FrameBgActive, theme.frame_active},
        {ImGuiCol_Button, theme.frame},            {ImGuiCol_ButtonHovered, theme.frame_hot},
        {ImGuiCol_ButtonActive, theme.frame_active}, {ImGuiCol_Header, ImVec4(theme.accent.x, theme.accent.y, theme.accent.z, 0.35f)},
        {ImGuiCol_HeaderHovered, theme.frame_hot}, {ImGuiCol_HeaderActive, theme.frame_active},
        {ImGuiCol_CheckMark, theme.accent},        {ImGuiCol_NavCursor, theme.accent},
        {ImGuiCol_Text, theme.text},               {ImGuiCol_TextDisabled, theme.dim}};
    for (const auto& [slot, color] : colors) {
        ImGui::PushStyleColor(slot, color);
    }
    const int pushed = static_cast<int>(std::size(colors));
    std::optional<fs::path> picked;
    if (!ImGui::BeginPopupModal(title, &keep, kFlags)) {
        active_ = false;
        ImGui::PopStyleColor(pushed);
        ImGui::PopFont();
        return std::nullopt;
    }

    const fs::path parent = folder_.parent_path();
    ImGui::BeginDisabled(parent.empty() || parent == folder_);
    if (ImGui::Button(Tr("Up"))) {
        Go(parent);
    }
    ImGui::EndDisabled();
    if (!folders) {
        ImGui::SameLine();
        if (ImGui::Checkbox(Tr("Show all files"), &show_all_)) {
            Go(folder_);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Utf8(folder_).c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float places_width = ImGui::GetContentRegionAvail().x * 0.24f;
    fs::path enter;
    if (ImGui::BeginChild("places", ImVec2(places_width, -footer), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < places_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(Tr(places_[i].label.c_str()), places_[i].path == folder_)) {
                enter = places_[i].path;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("files", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
        if (!listing_.readable) {
            ImGui::TextDisabled("%s", Tr("This folder cannot be read."));
        } else if (listing_.entries.empty()) {
            ImGui::TextDisabled("%s", Tr(show_all_ || folders ? "This folder is empty." : "No .pkg files or folders here."));
        }
        const float right_edge = ImGui::GetContentRegionAvail().x;
        if (!parent.empty() && parent != folder_ && ImGui::Selectable(Tr("../  (parent folder)"))) {
            enter = parent;
        }
        for (size_t i = 0; i < listing_.entries.size(); ++i) {
            const FolderEntry& entry = listing_.entries[i];
            if (focus_list_) {
                ImGui::SetKeyboardFocusHere();
                focus_list_ = false;
            }
            ImGui::PushID(static_cast<int>(i));
            const std::string label = entry.directory ? entry.name + "/" : entry.name;
            if (folders && !entry.directory) {
                ImGui::TextDisabled("%s", label.c_str());
            } else if (ImGui::Selectable(label.c_str())) {
                if (entry.directory) {
                    enter = folder_ / PathFromUtf8(entry.name);
                } else {
                    picked = folder_ / PathFromUtf8(entry.name);
                }
            }
            if (!entry.directory && !folders) {
                const std::string size = FormatSize(entry.size);
                ImGui::SameLine(right_edge - ImGui::CalcTextSize(size.c_str()).x);
                ImGui::TextDisabled("%s", size.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (ImGui::Button(Tr("Cancel"))) {
        keep = false;
    }
    ImGui::SameLine();
    if (folders) {
        if (ImGui::Button(Tr("Select this folder"))) {
            picked = folder_;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr("Open the folder with eboot.bin, sce_module and sce_sys, then select it. "
                                     "Controller: D-pad to move, A to open, B to cancel."));
    } else {
        ImGui::TextDisabled("%s",
                            Tr("Choose the .pkg file. Controller: D-pad to move, A to choose, B to cancel."));
    }

    if (!enter.empty()) {
        Go(enter);
    }
    if (picked || !keep) {
        ImGui::CloseCurrentPopup();
        active_ = false;
    }
    ImGui::EndPopup();
    ImGui::PopStyleColor(pushed);
    ImGui::PopFont();
    return picked;
}

} // namespace dcp
