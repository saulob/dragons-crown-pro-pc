// The pending edit of one SAVE0.DAT: the persisted save, a working copy, and the only path that writes the working copy.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "appearance_custom.h"
#include "hp_override.h"
#include "save_data.h"
#include "save_discovery.h"

namespace dcp {

struct InvalidAppearance {
    int slot = 0;
    int character = 0;
    std::string name;
    uint8_t value = 0;
};

// One line per character whose native colour differs between two saves.
std::vector<std::string> DescribeColorChanges(const DiscoveredSave& save, const SaveModel& before, const SaveModel& after);

class SaveEdit {
public:
    enum class Result { kApplied, kNothingToApply, kInvalidAppearance, kInvalidHp, kInvalidInventory, kInvalidSave, kStale, kBackupFailed, kWriteFailed };

    SaveEdit(DiscoveredSave save, std::filesystem::path backup_folder);

    const DiscoveredSave& save() const { return save_; }
    const SaveModel& persisted() const { return save_.model; }
    const SaveModel& working() const { return working_; }
    SaveModel& working() { return working_; }

    bool dirty() const;
    void Discard();
    std::optional<InvalidAppearance> FirstInvalidAppearance() const;

    // Validates, backs up, writes through a validated temporary file, replaces atomically and reads the result back.
    // Nothing on disk changes unless every step succeeds. kStale: the file no longer holds the bytes this edit was opened
    // on (a restore, an import or the game wrote it), so the working copy is never written over it.
    Result Apply();
    const std::string& detail() const { return detail_; }

private:
    DiscoveredSave save_;
    SaveModel working_;
    std::filesystem::path backup_folder_;
    std::string detail_;
};

struct ApplyOutcome {
    SaveEdit::Result save = SaveEdit::Result::kNothingToApply;
    bool custom_written = false;
    bool custom_failed = false;
    bool override_written = false;
    bool override_failed = false;
    bool rolled_back = false; // sidecar files were put back after another half could not be written
    bool ok() const {
        return !custom_failed && !override_failed &&
               (save == SaveEdit::Result::kApplied || save == SaveEdit::Result::kNothingToApply);
    }
};

// Writes the sidecar files, then the save; when any step fails, prior sidecar files are put back so neither half is
// reported applied on its own. `saved` and `override_saved` follow what is on disk afterwards.
ApplyOutcome ApplyAll(SaveEdit* edit, const CustomStore& working, CustomStore& saved,
                      const CharacterOverrideStore* override_working = nullptr,
                      CharacterOverrideStore* override_saved = nullptr);

} // namespace dcp
