// The pending edit of one SAVE0.DAT: the persisted save, a working copy, and the only path that writes the working copy.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "appearance_custom.h"
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
    enum class Result { kApplied, kNothingToApply, kInvalidAppearance, kInvalidSave, kBackupFailed, kWriteFailed };

    SaveEdit(DiscoveredSave save, std::filesystem::path backup_folder);

    const DiscoveredSave& save() const { return save_; }
    const SaveModel& persisted() const { return save_.model; }
    const SaveModel& working() const { return working_; }
    SaveModel& working() { return working_; }

    bool dirty() const;
    void Discard();
    std::optional<InvalidAppearance> FirstInvalidAppearance() const;

    // Validates, backs up, writes through a validated temporary file, replaces atomically and reads the result back.
    // Nothing on disk changes unless every step succeeds.
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
    bool rolled_back = false; // the custom file was put back after the save could not be written
    bool ok() const {
        return !custom_failed && (save == SaveEdit::Result::kApplied || save == SaveEdit::Result::kNothingToApply);
    }
};

// Writes the custom looks, then the save; when the save cannot be written the previous custom file is put back, so
// neither half is reported applied on its own. `saved` follows what is on disk afterwards.
ApplyOutcome ApplyAll(SaveEdit* edit, const CustomStore& working, CustomStore& saved);

} // namespace dcp
