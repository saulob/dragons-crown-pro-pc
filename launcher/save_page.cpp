#include "save_page.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <system_error>
#include <utility>

#include "editor_model.h"
#include "loading_view.h"
#include "localization/ui_text.h"
#include "page_margin.h"
#include "trg/launcher.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr const char* kMiddleDot = " \xC2\xB7 ";

void WarnText(trg::Ui& ui, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

std::string CountText(int count, const char* one, const char* many) {
    return std::to_string(count) + " " + Tr(count == 1 ? one : many);
}

const char* SourceTypeText(ImportSource source) {
    switch (source) {
    case ImportSource::kSaveFile: return "SAVE0.DAT file";
    case ImportSource::kFolder: return "Folder";
    case ImportSource::kZip: return "ZIP archive";
    case ImportSource::kRar: return "RAR archive";
    }
    return "SAVE0.DAT file";
}

std::string ImportSourceName(const ImportResult& found) {
    if (found.source == ImportSource::kZip || found.source == ImportSource::kRar) return Utf8(found.file.filename());
    const std::filesystem::path folder = found.file.parent_path().filename();
    return Utf8(folder.empty() ? found.file.filename() : folder);
}

enum class BackupClick { kNone, kRestore, kDelete };

BackupClick DrawBackupRow(const BackupRow& row, int index, bool can_restore, float button, float spacing) {
    BackupClick click = BackupClick::kNone;
    ImGui::PushID(index);
    const float start = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row.when.c_str());
    ImGui::SameLine(start + float(width * 0.42));
    ImGui::TextUnformatted(row.size.c_str());
    ImGui::SameLine(start + width - button - button - spacing);
    ImGui::BeginDisabled(!can_restore);
    if (ImGui::Button(Tr("Restore..."), ImVec2(button, 0))) click = BackupClick::kRestore;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr("Delete..."), ImVec2(button, 0))) click = BackupClick::kDelete;
    ImGui::PopID();
    return click;
}

void DrawBackupDivider(ImU32 color, float thickness) {
    const ImVec2 next = ImGui::GetCursorScreenPos();
    const float y = std::floor((ImGui::GetItemRectMax().y + next.y) / 2);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(next.x, y), ImVec2(next.x + ImGui::GetContentRegionAvail().x, y + thickness), color);
}

std::string WithCount(std::string text, const char* token, size_t value) {
    const size_t at = text.find(token);
    if (at != std::string::npos) text.replace(at, std::strlen(token), std::to_string(value));
    return text;
}

std::string WithDetail(const char* message, const std::string& detail) {
    return detail.empty() ? std::string(Tr(message)) : std::string(Tr(message)) + " (" + detail + ")";
}

// Outlines the whole import preview, from its heading to its action row, while the preview is open.
class PreviewOutline {
public:
    PreviewOutline(trg::Ui& ui, const std::optional<ImportPlan>& plan) : ui(ui), plan_(plan) {
        ui.EndRows();
        ImGui::BeginGroup();
    }
    ~PreviewOutline() {
        ui.EndRows();
        ImGui::EndGroup();
        if (!plan_) return;
        const float pad = 4.0f * ui.scale();
        ImGui::GetWindowDrawList()->AddRect(ImVec2(ImGui::GetItemRectMin().x - pad, ImGui::GetItemRectMin().y - pad),
                                            ImVec2(ImGui::GetItemRectMax().x + pad, ImGui::GetItemRectMax().y + pad),
                                            trg::Col(ui.theme().accent), 4.0f * ui.scale(), 0, 2.0f * ui.scale());
    }

private:
    trg::Ui& ui;
    const std::optional<ImportPlan>& plan_;
};

} // namespace

SavePage::SavePage(std::filesystem::path user_dir, SaveSelection& selection, SavePageHooks hooks)
    : user_dir_(std::move(user_dir)), selection_(selection), hooks_(std::move(hooks)) {}

void SavePage::Refresh() {
    const GameBuild* build = hooks_.installed_build ? hooks_.installed_build() : nullptr;
    installed_title_ = build ? build->title_id : "";
    installed_label_ = build ? build->label : "";
    saves_ = DiscoverProSaves(user_dir_, installed_title_);
    visible_ = SavesOfTitle(saves_, installed_title_);
    backups_ = ListBackups(BackupFolder(user_dir_));
    const std::filesystem::path preferred[] = {selection_.save0};
    Select(ChooseSave(saves_, visible_, preferred));
}

const DiscoveredSave* SavePage::Selected() const {
    return selected_ >= 0 && selected_ < int(saves_.size()) ? &saves_[selected_] : nullptr;
}

SaveGuards SavePage::Guards() const {
    return {hooks_.editor_dirty && hooks_.editor_dirty(), hooks_.game_active && hooks_.game_active()};
}

void SavePage::Select(int index) {
    selected_ = index;
    pending_export_.reset();
    pending_import_.reset();
    pending_backup_.reset();
    if (const DiscoveredSave* save = Selected()) selection_.save0 = save->save0_path;
    RebuildBackups();
}

void SavePage::RebuildBackups() {
    const DiscoveredSave* save = Selected();
    listing_ = save ? LatestBackups(backups_, *save) : BackupListing{};
}

void SavePage::Replaced(trg::Ui& ui, const char* message) {
    ++selection_.generation;
    pending_import_.reset();
    reload_ = true; // on the next frame: this one still draws from saves_
    ui.SetStatus(Tr(message), 6.0);
}

void SavePage::Draw(trg::Ui& ui) {
    if (activation_.Entered(ImGui::GetFrameCount())) {
        if (!loaded_ && !std::exchange(loading_shown_, true)) {
            DrawLoadingView(ui, "Loading saves...");
            activation_ = PageActivation();
            return;
        }
        Refresh();
        loaded_ = true;
    }
    if (std::exchange(reload_, false)) Refresh();
    if (!pending_status_.empty()) ui.SetStatus(std::exchange(pending_status_, std::string()), 8.0);
    if (installed_title_.empty()) {
        ui.StatusCard(trg::Status::kAttention, Tr("No installed game"),
                      Tr("Install the game on the Play page. The Saves page manages the saves of the installed game."));
        return;
    }
    ui.Info("Installed game", installed_label_.c_str());
    DrawSelector(ui);
    const DiscoveredSave* save = Selected();
    if (!save) {
        ui.StatusCard(trg::Status::kAttention, Tr("No saves found"),
                      Tr("Play the game once and save, then come back. Saves are kept in the user folder's savedata folder."));
        ui.EndRows();
        return;
    }
    DrawOverview(ui, *save);
    DrawSlots(ui, *save);
    DrawManagement(ui, save);
    DrawBackups(ui, *save);
    DrawDetails(ui, *save);
    ui.EndRows();
}

void SavePage::DrawSelector(trg::Ui& ui) {
    if (visible_.size() < 2) return;
    ui.Row("Selected save", "The save folders of the installed game. Choosing one never changes it.");
    const auto label = [](const DiscoveredSave& save) {
        std::string text = save.dir_name;
        if (ClassifySave(save.model) != SaveFileStatus::kValid) text += kMiddleDot + std::string(Tr("Invalid"));
        return text;
    };
    const DiscoveredSave* current = Selected();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##save_choice", current ? label(*current).c_str() : "")) {
        for (const size_t index : visible_) {
            ImGui::PushID(int(index));
            const bool chosen = int(index) == selected_;
            if (ImGui::Selectable(label(saves_[index]).c_str(), chosen) && !chosen) Select(int(index));
            if (chosen) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void SavePage::DrawOverview(trg::Ui& ui, const DiscoveredSave& save) {
    const SaveOverview overview = DescribeOverview(save);
    const bool valid = overview.status == SaveFileStatus::kValid;
    const std::string title = std::string(Tr(valid ? "Valid save" : "This save cannot be used")) + kMiddleDot + save.dir_name;
    const char* detail = valid ? Tr("This save belongs to the installed game.")
                               : Tr("It is shown for reference only: nothing on this page will back it up or replace it.");
    ui.StatusCard(valid ? trg::Status::kReady : trg::Status::kAttention, title.c_str(), detail);
    ui.Heading("Overview");
    ui.InfoGrid({{"Platform", overview.platform},
                 {"Save folder", overview.save_name},
                 {"Status", Tr(SaveStatusText(overview.status))},
                 {"CRC", Tr(CrcStatusText(overview.status))},
                 {"DATA slots", std::to_string(overview.populated_slots) + " / " + std::to_string(kDataSlotCount)},
                 {"Characters", std::to_string(overview.characters)},
                 {"Modified", overview.modified},
                 {"File size", overview.size}});
}

void SavePage::DrawSlots(trg::Ui& ui, const DiscoveredSave& save) {
    const std::vector<SlotOverview> slots = DescribeSlots(save.model);
    if (slots.empty()) return;
    ui.Heading("DATA Slots");
    for (const SlotOverview& slot : slots) {
        std::string names;
        for (const SlotCharacter& character : slot.characters) {
            if (!names.empty()) names += ", ";
            names += character.name + " (" + Tr(std::string(ClassName(character.klass))) + " " + Tr("Lv") + " " +
                     std::to_string(character.level) + ")";
        }
        const std::string value = slot.empty() ? std::string(Tr("Empty"))
                                               : CountText(int(slot.characters.size()), "character", "characters") + kMiddleDot +
                                                     FormatThousands(slot.gold) + " " + Tr("Gold");
        ui.Info(slot.name.c_str(), value.c_str());
        if (!names.empty()) ui.Help(names.c_str());
    }
    const bool editable = save.current && hooks_.open_editor;
    ImGui::BeginDisabled(!editable);
    if (ui.ButtonRow("Characters", editable ? "Names, levels, skill points, HP and appearance are edited on the Editor page."
                                            : "The Editor edits the saves of the installed game.",
                     "Open Editor")) {
        hooks_.open_editor();
    }
    ImGui::EndDisabled();
}

void SavePage::DrawManagement(trg::Ui& ui, const DiscoveredSave* save) {
    const SaveGuards guards = Guards();
    const ActionState backup = ActionStateFor(SaveAction::kBackup, save, guards);
    const ActionState exporting = ActionStateFor(SaveAction::kExport, save, guards);
    const ActionState importing = ActionStateFor(SaveAction::kImport, save, guards);
    ui.Heading("Save Management");
    ImGui::BeginDisabled(!backup.enabled);
    if (ui.ButtonRow("Create backup", "Copies this SAVE0.DAT into the backup folder. The save is not changed.", "Create backup") &&
        save) {
        const SaveOpResult result = BackupSave(*save, BackupFolder(user_dir_));
        if (result.ok) {
            backups_ = ListBackups(BackupFolder(user_dir_));
            RebuildBackups();
        }
        ui.SetStatus(result.ok ? std::string(Tr("Backup created.")) : WithDetail("Could not create the backup.", result.error), 6.0);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!exporting.enabled);
    if (ui.ButtonRow("Export PS4 save", "Copies this Dragon's Crown Pro SAVE0.DAT, byte for byte, into a folder you choose.",
                     "Export PS4 save...") &&
        hooks_.choose) {
        hooks_.choose(SavePick::kExportFolder);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!importing.enabled);
    const bool import_ps4 =
        ui.ButtonRow("Import PS4 save",
                     "Replaces this save with a Dragon's Crown Pro SAVE0.DAT, a folder holding one, or a ZIP of it, after a confirmation. "
                     "This save is backed up first.",
                     "Import PS4 save...");
    DrawImportMenu(ui, import_ps4, ImportMode::kPs4);
    const bool import_ps3 =
        ui.ButtonRow("Import PS3 save",
                     "Replaces this save with a Dragon's Crown PS3 save: its folder, or a ZIP of it, with SAVE0.DAT and PARAM.PFD. "
                     "It is decrypted and converted, and this save is backed up first.",
                     "Import PS3 save...");
    DrawImportMenu(ui, import_ps3, ImportMode::kPs3);
    ImGui::EndDisabled();
    if (ui.ButtonRow("Backups", "Where the backups of every save are kept.", "Open backup folder") && hooks_.open_folder) {
        std::error_code ec;
        std::filesystem::create_directories(BackupFolder(user_dir_), ec);
        hooks_.open_folder(BackupFolder(user_dir_));
    }
    const ActionState restore = ActionStateFor(SaveAction::kRestore, save, guards);
    const char* reason = *backup.reason ? backup.reason : restore.reason;
    if (save && *reason) {
        ui.EndRows();
        WarnText(ui, Tr(reason));
    }
    DrawExportConfirm(ui);
    DrawImportConfirm(ui);
}

void SavePage::DrawImportMenu(trg::Ui& ui, bool open, ImportMode mode) {
    const bool ps3 = mode == ImportMode::kPs3;
    const char* popup = ps3 ? "save_import_ps3_source" : "save_import_ps4_source";
    const ImVec2 button_min = ImGui::GetItemRectMin(), button_max = ImGui::GetItemRectMax();
    if (open) {
        if (hooks_.builtin_chooser && hooks_.builtin_chooser()) {
            if (hooks_.choose) hooks_.choose(ps3 ? SavePick::kImportPs3Folder : SavePick::kImportPs4Folder);
            return;
        }
        ImGui::OpenPopup(popup);
    }
    ImGui::SetNextWindowPos(ImVec2(button_min.x, button_max.y + 4.0f * ui.scale()), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(button_max.x - button_min.x, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup(popup)) return;
    std::optional<SavePick> chosen;
    if (ImGui::Selectable(Tr(ps3 ? "ZIP archive of a PS3 save..." : "SAVE0.DAT or ZIP archive..."))) {
        chosen = ps3 ? SavePick::kImportPs3File : SavePick::kImportPs4File;
    }
    ImGui::SetItemDefaultFocus();
    if (ImGui::Selectable(Tr(ps3 ? "PS3 save folder (SAVE0.DAT and PARAM.PFD)..." : "Folder containing SAVE0.DAT..."))) {
        chosen = ps3 ? SavePick::kImportPs3Folder : SavePick::kImportPs4Folder;
    }
    ImGui::EndPopup();
    if (chosen && hooks_.choose) hooks_.choose(*chosen);
}

void SavePage::Picked(SavePick pick, const std::filesystem::path& path) {
    const DiscoveredSave* save = Selected();
    if (path.empty() || !save) return;
    if (pick == SavePick::kExportFolder) {
        if (!ActionStateFor(SaveAction::kExport, save, Guards()).enabled) return;
        ExportPlan plan = PlanExport(*save, path, user_dir_);
        if (!plan.refused.empty()) {
            pending_status_ = Tr(plan.refused);
        } else if (plan.exists) {
            pending_export_ = std::move(plan);
        } else {
            const SaveOpResult result = ExportSave(*save, plan);
            pending_status_ = result.ok ? std::string(Tr("Save exported.")) : WithDetail("Could not export the save.", result.error);
        }
        return;
    }
    if (!ActionStateFor(SaveAction::kImport, save, Guards()).enabled) return;
    const bool ps3 = pick == SavePick::kImportPs3File || pick == SavePick::kImportPs3Folder;
    pending_import_ = PlanImport(path, *save, ps3 ? ImportMode::kPs3 : ImportMode::kPs4);
}

void SavePage::DrawExportConfirm(trg::Ui& ui) {
    const DiscoveredSave* save = Selected();
    if (!pending_export_ || !save) return;
    ui.Heading("Export PS4 save");
    const std::string question = std::string(Tr("The chosen folder already has a SAVE0.DAT. Replace it with a copy of this save?")) +
                                 "\n\n" + Utf8(pending_export_->destination);
    if (ui.ConfirmRow("Replace existing file", "The file in the chosen folder is replaced; the selected save is not changed.",
                      "Replace file...", question.c_str(), "Replace", "Cancel")) {
        const SaveOpResult result = ExportSave(*save, *pending_export_);
        pending_export_.reset();
        ui.SetStatus(result.ok ? std::string(Tr("Save exported.")) : WithDetail("Could not export the save.", result.error), 6.0);
        return;
    }
    if (ui.ButtonRow("Cancel export", nullptr, "Cancel")) pending_export_.reset();
}

void SavePage::DrawImportConfirm(trg::Ui& ui) {
    const DiscoveredSave* save = Selected();
    if (!pending_import_ || !save) return;
    const PreviewOutline outline(ui, pending_import_);
    const ImportResult& found = pending_import_->inspected;
    const bool ps3 = pending_import_->mode == ImportMode::kPs3;
    ui.Heading(ps3 ? "Import PS3 save" : "Import PS4 save");
    const bool blocked = !pending_import_->refused.empty();
    const std::string source = ImportSourceName(found);
    std::vector<std::pair<std::string, std::string>> rows = {{"Source", source}, {"Source type", Tr(SourceTypeText(found.source))}};
    if (!blocked) rows.push_back({"Save status", Tr("Supported")});
    if (found.data_slots > 0) {
        rows.push_back({"DATA slots", std::to_string(found.data_slots)});
        rows.push_back({"Characters", std::to_string(found.characters)});
    }
    rows.push_back({"Destination", pending_import_->destination_identity});
    ui.InfoGrid(rows);
    if (blocked) {
        WarnText(ui, Tr(pending_import_->refused));
        if (ui.ButtonRow("Dismiss", nullptr, "Dismiss")) pending_import_.reset();
        return;
    }
    if (pending_import_->unverified_platform) {
        WarnText(ui, Tr("PS Vita saves have not been verified with Dragon's Crown Pro. The file passes the Pro checks, "
                        "but the game may not read it as expected."));
    }
    const bool allowed = ActionStateFor(SaveAction::kImport, save, Guards()).enabled;
    const std::string question = std::string(Tr(ps3 ? "Replace the selected save with the converted PS3 save?" : "Replace the selected save with the imported SAVE0.DAT?")) +
                                 "\n\n" + Tr("Source") + ": " + source + "\n" + Tr("Replaces") + ": " + pending_import_->destination_identity + "\n\n" +
                                 Tr("The selected save is backed up first.");
    ImGui::BeginDisabled(!allowed);
    const bool confirmed = ui.ConfirmRow("Replace the selected save", "Your current save will be backed up before it is replaced",
                                         "Import and replace...", question.c_str(), "Import", "Cancel");
    ImGui::EndDisabled();
    if (confirmed && ActionStateFor(SaveAction::kImport, save, Guards()).enabled) {
        const SaveOpResult result = CommitImportPlan(*pending_import_, *save, BackupFolder(user_dir_));
        if (result.ok) {
            Replaced(ui, "Save imported. The previous save was backed up.");
        } else {
            pending_import_.reset();
            reload_ = result.stale;
            ui.SetStatus(WithDetail("Import failed; the selected save was not changed.", result.error), 8.0);
        }
        return;
    }
    if (ui.ButtonRow("Cancel import", nullptr, "Cancel")) pending_import_.reset();
}

void SavePage::DrawBackups(trg::Ui& ui, const DiscoveredSave& save) {
    ui.Heading("Backups");
    if (listing_.total == 0) {
        ui.Help("No backups of this save yet. The launcher also makes one before every change it writes.");
        return;
    }
    const std::string count = listing_.total > listing_.shown.size()
                                  ? WithCount(WithCount(Tr("Showing latest {shown} of {total}"), "{shown}", listing_.shown.size()), "{total}",
                                              listing_.total)
                                  : CountText(int(listing_.total), "backup", "backups");
    ui.Help(count.c_str());
    ui.EndRows();

    constexpr double kVisibleRows = 6.5;
    const float row_height = ImGui::GetFrameHeightWithSpacing() + 2.0f * ui.scale();
    const bool scrolls = listing_.shown.size() > size_t(kVisibleRows);
    const float rows = scrolls ? float(kVisibleRows) : float(listing_.shown.size());
    const float height = rows * row_height + 12.0f * ui.scale();
    const float button = 104.0f * ui.scale();
    const float spacing = 8.0f * ui.scale();
    constexpr int kDividerPixels = 1;
    const ImU32 divider = trg::Col(ui.theme().border_row);
    const float outer_gutter = std::floor(trg::layout::kContentPadding * ui.scale());
    const bool allowed = ActionStateFor(SaveAction::kRestore, &save, Guards()).enabled;
    std::optional<PendingBackup> chosen;
    const ImGuiWindowFlags scroll = scrolls ? ImGuiWindowFlags_AlwaysVerticalScrollbar : ImGuiWindowFlags_None;
    if (ImGui::BeginChild("##backup_list", ImVec2(-outer_gutter, height), ImGuiChildFlags_Borders, scroll)) {
        if (!scrolls) ReserveScrollbarWidth();
        KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));
        for (size_t i = 0; i < listing_.shown.size(); ++i) {
            const BackupRow& row = listing_.shown[i];
            if (i > 0) DrawBackupDivider(divider, float(kDividerPixels));
            const BackupClick click = DrawBackupRow(row, int(i), allowed, button, spacing);
            if (click != BackupClick::kNone) chosen = PendingBackup{row.info, row.when, click == BackupClick::kDelete};
        }
    }
    ImGui::EndChild();
    if (chosen) {
        pending_backup_ = std::move(chosen);
        open_backup_popup_ = true;
    }
    DrawBackupConfirm(ui, save);
}

void SavePage::DrawBackupConfirm(trg::Ui& ui, const DiscoveredSave& save) {
    constexpr char kPopup[] = "##backup_confirm";
    if (std::exchange(open_backup_popup_, false)) ImGui::OpenPopup(kPopup);
    if (!pending_backup_) return;
    const bool open = ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                                                  ImGuiWindowFlags_NoSavedSettings);
    if (!open) {
        pending_backup_.reset();
        return;
    }
    const PendingBackup& pending = *pending_backup_;
    const std::string question = pending.remove
                                     ? std::string(Tr("Delete backup")) + "\n\n" + pending.when + "\n\n" + Tr("This permanently deletes this backup")
                                     : std::string(Tr("Replace the selected save with this backup?")) + "\n\n" + Tr("Backup") + ": " + pending.when +
                                           "\n" + Tr("Replaces") + ": " + SaveIdentity(save) + "\n\n" + Tr("The selected save is backed up first.");
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted(question.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(320.0f * ui.scale(), 6.0f * ui.scale()));
    const bool confirmed = ui.AccentButton(Tr(pending.remove ? "Delete backup" : "Restore"), ImVec2(150.0f * ui.scale(), 0.0f));
    ImGui::SameLine();
    const bool cancelled = ImGui::Button(Tr("Cancel"), ImVec2(150.0f * ui.scale(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (confirmed || cancelled) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (!confirmed) {
        if (cancelled) pending_backup_.reset();
        return;
    }
    const PendingBackup done = std::move(*pending_backup_);
    pending_backup_.reset();
    if (done.remove) {
        const SaveOpResult result = DeleteSaveBackup(done.info, save, BackupFolder(user_dir_));
        backups_ = ListBackups(BackupFolder(user_dir_));
        RebuildBackups();
        ui.SetStatus(result.ok ? std::string(Tr("Backup deleted.")) : WithDetail("Delete failed", result.error), 6.0);
        return;
    }
    if (!ActionStateFor(SaveAction::kRestore, &save, Guards()).enabled) return;
    const SaveOpResult result = RestoreToSave(done.info, save, BackupFolder(user_dir_));
    if (result.ok) {
        Replaced(ui, "Backup restored. The previous save was backed up.");
    } else {
        reload_ = result.stale;
        ui.SetStatus(WithDetail("Restore failed; the selected save was not changed.", result.error), 8.0);
    }
}

void SavePage::DrawDetails(trg::Ui& ui, const DiscoveredSave& save) {
    ui.Heading("Details");
    ui.Info("File", Utf8(save.save0_path).c_str());
    ui.Info("Backup folder", Utf8(BackupFolder(user_dir_)).c_str());
}

} // namespace dcp
