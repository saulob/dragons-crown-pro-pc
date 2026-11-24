#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "appearance_custom.h"
#include "save_data.h"

namespace dcp {

// user/character_overrides.dat. Written atomically; an unreadable or newer file is never overwritten.
class HpOverrideStore {
public:
    explicit HpOverrideStore(std::filesystem::path path) : path_(std::move(path)) {}
    bool Load();
    bool Save() const;
    const std::filesystem::path& path() const { return path_; }
    const std::string& error() const { return error_; }

    std::optional<uint16_t> Get(const CustomKey& key) const;
    void Set(const CustomKey& key, std::optional<uint16_t> max_hp);
    void Clear(const CustomKey& key) { Set(key, std::nullopt); }
    const std::vector<std::pair<CustomKey, uint16_t>>& entries() const { return entries_; }
    bool operator==(const HpOverrideStore& other) const { return entries_ == other.entries_; }

    std::vector<uint8_t> Serialize() const;
    bool Parse(std::span<const uint8_t> bytes);

private:
    std::filesystem::path path_;
    std::vector<std::pair<CustomKey, uint16_t>> entries_;
    mutable std::string error_;
};

std::filesystem::path HpOverrideStorePath(const std::filesystem::path& user_dir);

// Mirrors the Editor's Max HP for every character it may override: a changed Max HP is stored and an existing entry is
// refreshed, while an untouched character without an entry stays native.
void SyncHpOverrides(HpOverrideStore& store, const SaveModel& working, const SaveModel& persisted,
                     std::string_view title_id, std::string_view save_dir);

} // namespace dcp
