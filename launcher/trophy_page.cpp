#include "trophy_page.h"

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <vector>

#include "dcp_png.h"
#include "trg/platform.h"
#include "host.h"
#include "utf8.h"
#include "trophies/trophy_store.h"

namespace fs = std::filesystem;

namespace dcp {
namespace {

constexpr int kIconsPerFrame = 6;
constexpr uintmax_t kMaxIconBytes = 4u << 20;

ImU32 GradeColor(trophies::Grade grade) {
    switch (grade) {
    case trophies::Grade::kPlatinum: return IM_COL32(150, 205, 235, 255);
    case trophies::Grade::kGold: return IM_COL32(232, 190, 64, 255);
    case trophies::Grade::kSilver: return IM_COL32(190, 196, 208, 255);
    case trophies::Grade::kBronze: return IM_COL32(205, 130, 70, 255);
    case trophies::Grade::kUnknown: break;
    }
    return IM_COL32(140, 145, 160, 255);
}

std::string UnlockDate(int64_t unix_seconds) {
    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) {
        return {};
    }
#else
    if (!localtime_r(&time, &local)) {
        return {};
    }
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
    return text;
}

const char* NoListReason(trophies::PackStatus status) {
    if (status == trophies::PackStatus::kNoPack) {
        return "This game folder has no trophy pack. Install the game again.";
    }
    return "The game's trophy pack could not be read. Install the game again.";
}

} // namespace

void TrophyPage::SetGame(const fs::path& app0) {
    if (app0 != app0_) {
        app0_ = app0;
        content_.reset();
    }
}

void TrophyPage::Invalidate() {
    content_.reset();
}

ImTextureID TrophyPage::Icon(int id) {
    const fs::path path = trophies::TrophyIconPath(user_dir_, content_->np_comm_id, id);
    const auto cached = icons_.find(path);
    if (cached != icons_.end()) {
        return cached->second;
    }
    if (icons_budget_ <= 0) {
        return ImTextureID{};
    }
    --icons_budget_;
    ImTextureID texture{};
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (!ec && size > 0 && size <= kMaxIconBytes) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        DcpPng::Image image;
        if (DcpPng::DecodeRgba(bytes.data(), bytes.size(), image)) {
            texture = host_.CreateTexture(image.rgba.data(), image.width, image.height);
        }
    }
    icons_[path] = texture;
    return texture;
}

void TrophyPage::Draw(trg::Ui& ui) {
    const int frame = ImGui::GetFrameCount();
    if (!content_ || frame > last_frame_ + 1) {
        content_ = LoadTrophyPage(app0_, user_dir_);
        if (content_->state == TrophyPageState::kList) {
            std::printf("Launcher: trophies %s, %d/%d unlocked\n", content_->np_comm_id.c_str(), content_->view.unlocked,
                        content_->view.total);
        }
    }
    last_frame_ = frame;
    icons_budget_ = kIconsPerFrame;
    DrawBody(ui);
    DrawModals(ui);
}

void TrophyPage::DrawBody(trg::Ui& ui) {
    switch (content_->state) {
    case TrophyPageState::kNoGame:
        ui.Paragraph("Install a supported game to view its trophies.");
        return;
    case TrophyPageState::kNoList:
        if (content_->pack == trophies::PackStatus::kKeyMissing) {
            DrawKeySetup(ui, "Trophy metadata requires a Trophy Key.",
                         "Dragon's Crown Pro PC does not include or generate this key.",
                         "You can enter your own key or import it from an existing shadPS4 installation.");
        } else if (content_->pack == trophies::PackStatus::kKeyRejected) {
            DrawKeySetup(ui, "The Trophy Key could not decrypt this game's trophy metadata.",
                         "Change the key, import another one, or remove it.", nullptr);
        } else {
            ui.StatusCard(trg::Status::kAttention, "Trophy list unavailable", NoListReason(content_->pack));
        }
        DrawSavedUnlocks(ui);
        return;
    case TrophyPageState::kList:
        break;
    }
    if (content_->key_configured) {
        DrawKeyRow(ui);
    }
    DrawSummary(ui);
    const float width = ImGui::GetContentRegionAvail().x;
    for (const trophies::TrophyRow& row : content_->view.rows) {
        ImGui::PushID(row.id);
        DrawRow(ui, row, width);
        ImGui::PopID();
    }
}

void TrophyPage::DrawKeySetup(trg::Ui& ui, const char* title, const char* detail, const char* more) {
    ui.StatusCard(trg::Status::kAttention, title, detail);
    if (more) {
        ui.Paragraph(more);
    }
    ui.Spacer(6);
    if (content_->key_configured) {
        DrawKeyRow(ui);
    } else if (ui.AccentButton("Enter Trophy Key", ImVec2(240 * ui.scale(), 0))) {
        entry_error_.clear();
        modal_ = Modal::kEnter;
        open_modal_ = true;
    }
    if (ImGui::Button("Import from shadPS4...", ImVec2(240 * ui.scale(), 0))) {
        ImportKey();
    }
    ui.Spacer(8);
}

void TrophyPage::DrawKeyRow(trg::Ui& ui) {
    const float scale = ui.scale();
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted("Trophy Key");
    ImGui::PopFont();
    ImGui::SameLine(0, 16 * scale);
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    ImGui::TextUnformatted("Configured");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 24 * scale);
    if (ImGui::Button("Change...")) {
        entry_error_.clear();
        modal_ = Modal::kEnter;
        open_modal_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        modal_ = Modal::kRemove;
        open_modal_ = true;
    }
    ui.Spacer(6);
}

void TrophyPage::ShowNotice(std::string text) {
    notice_ = std::move(text);
    modal_ = Modal::kNotice;
    open_modal_ = true;
}

void TrophyPage::ImportKey() {
#ifndef _WIN32
    if (std::system("command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1") != 0) {
        ShowNotice("No file chooser (zenity or kdialog) is installed, so a key file cannot be imported here. Use Enter Trophy Key instead.");
        return;
    }
#endif
    const std::string chosen = trg::BrowseForFile("Select a shadPS4 keys.json or config.toml",
                                                  {{"shadPS4 key files", "*.json;*.toml"}});
    if (chosen.empty()) {
        return;
    }
    if (const std::optional<trophies::TrophyKey> key = trophies::KeyFromFile(PathFromUtf8(chosen))) {
        OfferKey(*key, trophies::KeySource::kImported);
        return;
    }
    ShowNotice("No valid Trophy Key was found in this file.");
}

void TrophyPage::OfferKey(const trophies::TrophyKey& key, trophies::KeySource source) {
    if (!content_ || !content_->key_configured) {
        StoreKey(key, source);
        return;
    }
    pending_key_ = key;
    pending_source_ = source;
    modal_ = Modal::kReplace;
    open_modal_ = true;
}

void TrophyPage::StoreKey(const trophies::TrophyKey& key, trophies::KeySource source) {
    if (!trophies::StoreTrophyKey(user_dir_, key, source)) {
        ShowNotice("The Trophy Key could not be saved. Check that the user folder can be written.");
        return;
    }
    Invalidate();
}

void TrophyPage::DrawModals(trg::Ui& ui) {
    if (modal_ == Modal::kNone) {
        return;
    }
    const float scale = ui.scale();
    const char* id = "##trophy_key_dialog";
    if (open_modal_) {
        ImGui::OpenPopup(id);
        open_modal_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 20 * scale));
    const bool open = ImGui::BeginPopupModal(id, nullptr,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) {
        return;
    }
    const Modal shown = modal_;
    const auto close = [&] {
        modal_ = Modal::kNone;
        std::memset(entry_, 0, sizeof(entry_));
        show_key_ = false;
        pending_key_.reset();
        ImGui::CloseCurrentPopup();
    };
    const ImVec2 button(150 * scale, 0);
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    switch (shown) {
    case Modal::kEnter: ImGui::TextUnformatted("Enter Trophy Key"); break;
    case Modal::kReplace: ImGui::TextUnformatted("A Trophy Key is already configured. Replace it?"); break;
    case Modal::kRemove: ImGui::TextUnformatted("Remove the Trophy Key? Your trophies are kept."); break;
    case Modal::kNotice: ImGui::TextUnformatted(notice_.c_str()); break;
    case Modal::kNone: break;
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(380 * scale, 6 * scale));
    if (shown == Modal::kEnter) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##key", entry_, sizeof(entry_), show_key_ ? 0 : ImGuiInputTextFlags_Password);
        ImGui::Checkbox("Show key", &show_key_);
        if (!entry_error_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(entry_error_.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        if (ui.AccentButton("Save", button)) {
            if (const std::optional<trophies::TrophyKey> key = trophies::ParseKeyHex(entry_)) {
                const trophies::TrophyKey value = *key;
                entry_error_.clear();
                close();
                OfferKey(value, trophies::KeySource::kEntered);
            } else {
                entry_error_ = "Enter exactly 32 hexadecimal characters (0-9, A-F).";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button) || escape) {
            entry_error_.clear();
            close();
        }
    } else if (shown == Modal::kReplace) {
        if (ui.AccentButton("Replace", button)) {
            const std::optional<trophies::TrophyKey> key = pending_key_;
            const trophies::KeySource source = pending_source_;
            close();
            if (key) {
                StoreKey(*key, source);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button) || escape) {
            close();
        }
    } else if (shown == Modal::kRemove) {
        if (ui.AccentButton("Remove", button)) {
            close();
            if (trophies::ClearTrophyKey(user_dir_)) {
                Invalidate();
            } else {
                ShowNotice("The Trophy Key could not be removed. Check that the user folder can be written.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button) || escape) {
            close();
        }
    } else if (ui.AccentButton("OK", button) || escape) {
        close();
    }
    ImGui::EndPopup();
}

void TrophyPage::DrawSummary(trg::Ui& ui) {
    const trophies::TrophyView& view = content_->view;
    const float scale = ui.scale();
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::Text("%d / %d unlocked     %d%%", view.unlocked, view.total, view.percent);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui.theme().accent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ui.theme().frame);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * scale);
    ImGui::ProgressBar(view.total > 0 ? static_cast<float>(view.unlocked) / static_cast<float>(view.total) : 0.0f,
                       ImVec2(-FLT_MIN, 10 * scale), "");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    std::string grades;
    for (const trophies::Grade grade : {trophies::Grade::kPlatinum, trophies::Grade::kGold, trophies::Grade::kSilver,
                                        trophies::Grade::kBronze}) {
        const size_t index = static_cast<size_t>(grade);
        if (view.total_by_grade[index] == 0) {
            continue;
        }
        if (!grades.empty()) {
            grades += "     ";
        }
        grades += std::string(trophies::GradeName(grade)) + " " + std::to_string(view.unlocked_by_grade[index]) + "/" +
                  std::to_string(view.total_by_grade[index]);
    }
    ui.Help(grades.c_str());
    ui.Spacer(6);
}

void TrophyPage::DrawSavedUnlocks(trg::Ui& ui) {
    if (content_->saved_unlocks.empty()) {
        return;
    }
    const trg::Theme& theme = ui.theme();
    const float s = ui.scale();
    ui.Spacer(4);
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted("Saved trophies");
    ImGui::PopFont();
    ui.Paragraph("Names, icons and grades appear once the trophy list can be read.");
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 44 * s;
    for (const SavedUnlock& saved : content_->saved_unlocks) {
        ImGui::PushID(saved.id);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), trg::Col(theme.frame), 8 * s);
        char label[48];
        std::snprintf(label, sizeof(label), "Trophy %d", saved.id);
        draw->AddText(ui.fonts().semibold, ImGui::GetFontSize(), ImVec2(origin.x + 16 * s, origin.y + 12 * s),
                      trg::Col(theme.dim), label);
        if (saved.unix_time > 0) {
            const std::string date = "UNLOCKED  " + UnlockDate(saved.unix_time);
            const ImVec2 size = ui.fonts().semibold->CalcTextSizeA(ImGui::GetFontSize() * 0.8f, FLT_MAX, 0, date.c_str());
            draw->AddText(ui.fonts().semibold, ImGui::GetFontSize() * 0.8f,
                          ImVec2(origin.x + width - size.x - 16 * s, origin.y + 14 * s), trg::Col(theme.dim), date.c_str());
        }
        ImGui::Dummy(ImVec2(width, height + 6 * s));
        ImGui::PopID();
    }
}

void TrophyPage::DrawRow(trg::Ui& ui, const trophies::TrophyRow& row, float width) {
    const trg::Theme& theme = ui.theme();
    const trg::Fonts& fonts = ui.fonts();
    const float s = ui.scale();
    const float base = ImGui::GetFontSize();
    const float height = 82 * s, icon_size = 58 * s, gap = 8 * s;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end(origin.x + width, origin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, end, trg::Col(row.unlocked ? theme.frame_hot : theme.frame), 10 * s);
    if (row.unlocked) {
        draw->AddRectFilled(origin, ImVec2(origin.x + 4 * s, end.y), trg::Col(theme.accent), 10 * s,
                            ImDrawFlags_RoundCornersLeft);
    }
    ImGui::Selectable("##trophy", false, 0, ImVec2(width, height));
    ImGui::Dummy(ImVec2(0, gap));

    const ImVec2 icon_min(origin.x + 14 * s, origin.y + (height - icon_size) * 0.5f);
    const ImVec2 icon_max(icon_min.x + icon_size, icon_min.y + icon_size);
    const ImTextureID texture = row.masked ? ImTextureID{} : Icon(row.id);
    if (texture != ImTextureID{}) {
        draw->AddImageRounded(ImTextureRef(texture), icon_min, icon_max, ImVec2(0, 0), ImVec2(1, 1),
                              row.unlocked ? IM_COL32_WHITE : IM_COL32(105, 110, 125, 200), 8 * s);
    } else {
        draw->AddRectFilled(icon_min, icon_max, trg::Col(theme.frame_active), 8 * s);
        draw->AddRect(icon_min, icon_max, GradeColor(row.grade), 8 * s, 0, 1.5f * s);
    }

    const float text_x = icon_max.x + 14 * s;
    const float right = end.x - 14 * s;
    draw->PushClipRect(origin, end, true);
    const char* grade = trophies::GradeName(row.grade);
    const ImVec2 grade_size = fonts.semibold->CalcTextSizeA(base * 0.9f, FLT_MAX, 0, grade);
    draw->AddText(fonts.semibold, base * 0.9f, ImVec2(right - grade_size.x, origin.y + 12 * s), GradeColor(row.grade), grade);
    draw->AddText(fonts.semibold, base, ImVec2(text_x, origin.y + 10 * s),
                  trg::Col(row.unlocked ? theme.text : theme.dim), row.name.c_str(), nullptr,
                  right - grade_size.x - 12 * s - text_x);
    draw->AddText(fonts.regular, base * 0.84f, ImVec2(text_x, origin.y + 34 * s), trg::Col(theme.dim), row.detail.c_str(),
                  nullptr, right - text_x);
    if (row.unlocked) {
        const std::string date = row.unlock_time > 0 ? "UNLOCKED  " + UnlockDate(row.unlock_time) : "UNLOCKED";
        draw->AddText(fonts.semibold, base * 0.72f, ImVec2(text_x, end.y - 22 * s), trg::Col(theme.accent), date.c_str());
    }
    draw->PopClipRect();
}

} // namespace dcp
