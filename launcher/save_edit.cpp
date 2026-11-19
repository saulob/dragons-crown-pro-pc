#include "save_edit.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "save_backup.h"

namespace dcp {

std::vector<std::string> DescribeColorChanges(const DiscoveredSave& save, const SaveModel& before, const SaveModel& after) {
    std::vector<std::string> lines;
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        for (size_t index = 0; index < kCharacterCount; ++index) {
            const CharacterView& old_view = before.slots()[slot].characters[index];
            const CharacterView& new_view = after.slots()[slot].characters[index];
            if (new_view.present && old_view.color != new_view.color) {
                lines.push_back(save.title_id + " DATA " + std::to_string(slot + 1) + " slot " + std::to_string(index + 1) +
                                " " + std::string(ClassName(new_view.klass)) + " " + new_view.name + " native colour " +
                                std::to_string(old_view.color) + " -> " + std::to_string(new_view.color));
            }
        }
    }
    return lines;
}

SaveEdit::SaveEdit(DiscoveredSave save, std::filesystem::path backup_folder)
    : save_(std::move(save)), working_(save_.model), backup_folder_(std::move(backup_folder)) {}

bool SaveEdit::dirty() const {
    return !std::ranges::equal(working_.bytes(), save_.model.bytes());
}

void SaveEdit::Discard() {
    working_ = save_.model;
}

std::optional<InvalidAppearance> SaveEdit::FirstInvalidAppearance() const {
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        for (const CharacterView& character : working_.slots()[slot].characters) {
            if (character.present && !character.color_valid()) {
                return InvalidAppearance{int(slot), character.slot, character.name, character.color};
            }
        }
    }
    return std::nullopt;
}

SaveEdit::Result SaveEdit::Apply() {
    detail_.clear();
    if (!dirty()) {
        return Result::kNothingToApply;
    }
    if (FirstInvalidAppearance()) {
        return Result::kInvalidAppearance;
    }
    const std::vector<uint8_t> serialized = working_.Serialize();
    std::vector<std::string> problems;
    if (!ValidateSave(serialized, problems)) {
        detail_ = problems.empty() ? std::string("invalid save") : problems.front();
        return Result::kInvalidSave;
    }
    if (!CreateBackup(backup_folder_, save_.save0_path, save_.title_id, save_.dir_name)) {
        return Result::kBackupFailed;
    }
    if (const std::optional<std::string> error = WriteSaveAtomically(save_.save0_path, serialized, ValidateSave)) {
        detail_ = *error;
        return Result::kWriteFailed;
    }
    SaveModel reread = SaveModel::FromFile(save_.save0_path);
    if (!reread.valid() || !std::ranges::equal(reread.bytes(), serialized)) {
        detail_ = "the written save did not read back identically";
        return Result::kWriteFailed;
    }
    save_.model = std::move(reread);
    working_ = save_.model;
    return Result::kApplied;
}

namespace {

std::optional<std::vector<uint8_t>> Snapshot(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Restore(const std::filesystem::path& path, const std::optional<std::vector<uint8_t>>& previous) {
    std::error_code ec;
    if (!previous) {
        std::filesystem::remove(path, ec);
        return !std::filesystem::exists(path, ec);
    }
    CustomStore store(path);
    return store.Parse(*previous) && store.Save();
}

} // namespace

ApplyOutcome ApplyAll(SaveEdit* edit, const CustomStore& working, CustomStore& saved) {
    ApplyOutcome outcome;
    const bool custom_dirty = !(working == saved);
    const std::optional<std::vector<uint8_t>> previous = custom_dirty ? Snapshot(saved.path()) : std::nullopt;
    if (custom_dirty) {
        if (!working.Save()) {
            outcome.custom_failed = true;
            return outcome;
        }
        outcome.custom_written = true;
    }
    if (edit && edit->dirty()) {
        outcome.save = edit->Apply();
        if (outcome.save != SaveEdit::Result::kApplied) {
            if (custom_dirty) {
                outcome.rolled_back = Restore(saved.path(), previous);
                outcome.custom_written = false;
            }
            return outcome;
        }
    }
    if (custom_dirty) {
        CustomStore reread(saved.path());
        if (reread.Load() && reread == working) {
            saved = working;
        } else {
            outcome.custom_failed = true;
        }
    }
    return outcome;
}

} // namespace dcp
