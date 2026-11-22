// The launcher's Save page: the SAVE0.DAT files of the Pro builds, their DATA slots, backups, export, import and restore.
// Selecting never writes; every replacement is confirmed and backs the save up first. Characters are edited on the Editor page.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "game_profile.h"
#include "page_activation.h"
#include "save_backup.h"
#include "save_discovery.h"
#include "save_page_model.h"

namespace trg {
class Ui;
}

namespace dcp {

enum class SavePick { kImportPs4File, kImportPs4Folder, kImportPs3File, kImportPs3Folder, kExportFolder };

struct SavePageHooks {
    // Opens a chooser; the chosen path comes back through SavePage::Picked, now or on a later frame.
    std::function<void(SavePick)> choose;
    std::function<bool()> builtin_chooser; // the launcher's own chooser picks folders only
    std::function<void(const std::filesystem::path&)> open_folder;
    std::function<void()> open_editor;
    std::function<bool()> editor_dirty;
    std::function<bool()> game_active;
    std::function<const GameBuild*()> installed_build;
};

class SavePage {
public:
    SavePage(std::filesystem::path user_dir, SaveSelection& selection, SavePageHooks hooks);

    void Draw(trg::Ui& ui);

    // The status of what a chosen path led to is shown on the next Draw.
    void Picked(SavePick pick, const std::filesystem::path& path);

private:
    void Refresh();
    const DiscoveredSave* Selected() const;
    SaveGuards Guards() const;
    void Select(int index);
    void Replaced(trg::Ui& ui, const char* message);

    void DrawSelector(trg::Ui& ui);
    void DrawOverview(trg::Ui& ui, const DiscoveredSave& save);
    void DrawSlots(trg::Ui& ui, const DiscoveredSave& save);
    void DrawManagement(trg::Ui& ui, const DiscoveredSave* save);
    void DrawImportMenu(trg::Ui& ui, bool open, ImportMode mode);
    void DrawExportConfirm(trg::Ui& ui);
    void DrawImportConfirm(trg::Ui& ui);
    void DrawBackups(trg::Ui& ui, const DiscoveredSave& save);
    void DrawBackupConfirm(trg::Ui& ui, const DiscoveredSave& save);
    void RebuildBackups();
    void DrawDetails(trg::Ui& ui, const DiscoveredSave& save);

    std::filesystem::path user_dir_;
    SaveSelection& selection_;
    SavePageHooks hooks_;
    PageActivation activation_;
    bool loaded_ = false;
    bool loading_shown_ = false;
    bool reload_ = false;
    std::string installed_title_;
    std::string installed_label_;
    std::vector<DiscoveredSave> saves_;
    std::vector<size_t> visible_;
    std::vector<BackupInfo> backups_;
    BackupListing listing_;
    struct PendingBackup {
        BackupInfo info;
        std::string when;
        bool remove = false;
    };
    std::optional<PendingBackup> pending_backup_;
    bool open_backup_popup_ = false;
    int selected_ = -1;
    std::optional<ExportPlan> pending_export_;
    std::optional<ImportPlan> pending_import_;
    std::string pending_status_;
};

} // namespace dcp
