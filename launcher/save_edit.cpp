#include "save_edit.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "inventory_data.h"
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
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        for (size_t index = 0; index < kCharacterCount; ++index) {
            const CharacterView& before = save_.model.slots()[slot].characters[index];
            const CharacterView& after = working_.slots()[slot].characters[index];
            const bool hp_edited = before.hp_current != after.hp_current || before.hp_max != after.hp_max;
            if (after.present && hp_edited && after.hp_current > after.hp_max) {
                detail_ = "the current HP of a character is above its maximum HP";
                return Result::kInvalidHp;
            }
        }
    }
    if (std::string error; !ValidateItemChanges(save_.model.bytes(), working_.bytes(), error)) {
        detail_ = std::move(error);
        return Result::kInvalidInventory;
    }
    const std::vector<uint8_t> serialized = working_.Serialize();
    std::vector<std::string> problems;
    if (!ValidateSave(serialized, problems)) {
        detail_ = problems.empty() ? std::string("invalid save") : problems.front();
        return Result::kInvalidSave;
    }
    const SaveModel on_disk = SaveModel::FromFile(save_.save0_path);
    if (!std::ranges::equal(on_disk.bytes(), save_.model.bytes())) {
        detail_ = "the save changed on disk since it was loaded";
        return Result::kStale;
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

bool RestoreHp(const std::filesystem::path& path, const std::optional<std::vector<uint8_t>>& previous) {
    std::error_code ec;
    if (!previous) {
        std::filesystem::remove(path, ec);
        return !std::filesystem::exists(path, ec);
    }
    HpOverrideStore store(path);
    return store.Parse(*previous) && store.Save();
}

} // namespace

ApplyOutcome ApplyAll(SaveEdit* edit, const CustomStore& working, CustomStore& saved,
                      const HpOverrideStore* hp_working, HpOverrideStore* hp_saved) {
    ApplyOutcome outcome;
    const bool custom_dirty = !(working == saved);
    const bool hp_dirty = hp_working && hp_saved && !(*hp_working == *hp_saved);
    const std::optional<std::vector<uint8_t>> prev_custom = custom_dirty ? Snapshot(saved.path()) : std::nullopt;
    const std::optional<std::vector<uint8_t>> prev_hp = hp_dirty ? Snapshot(hp_saved->path()) : std::nullopt;
    auto rollback = [&]() {
        bool ok = true;
        if (outcome.custom_written) {
            ok &= Restore(saved.path(), prev_custom);
            outcome.custom_written = false;
        }
        if (outcome.hp_written) {
            ok &= RestoreHp(hp_saved->path(), prev_hp);
            outcome.hp_written = false;
        }
        outcome.rolled_back = ok;
    };
    if (custom_dirty) {
        if (!working.Save()) {
            outcome.custom_failed = true;
            return outcome;
        }
        outcome.custom_written = true;
    }
    if (hp_dirty) {
        if (!hp_working->Save()) {
            outcome.hp_failed = true;
            if (outcome.custom_written) rollback();
            return outcome;
        }
        outcome.hp_written = true;
    }
    if (edit && edit->dirty()) {
        outcome.save = edit->Apply();
        if (outcome.save != SaveEdit::Result::kApplied) {
            if (outcome.custom_written || outcome.hp_written) rollback();
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
    if (hp_dirty) {
        HpOverrideStore reread(hp_saved->path());
        if (reread.Load() && reread == *hp_working) {
            *hp_saved = *hp_working;
        } else {
            outcome.hp_failed = true;
        }
    }
    return outcome;
}

} // namespace dcp
