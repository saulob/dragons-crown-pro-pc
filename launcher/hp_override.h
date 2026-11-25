#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "appearance_custom.h"
#include "hp_override_hooks.h"
#include "save_data.h"

namespace dcp {

// One character's persisted runtime overrides. `present` names the fields the user made authoritative.
struct CharacterOverride {
    uint16_t present = 0;
    uint16_t max_hp = 0;
    uint16_t strength = 0;
    uint16_t constitution = 0;
    uint16_t dexterity = 0;
    uint16_t intelligence = 0;
    uint16_t magic_resistance = 0;
    uint16_t luck = 0;
    std::array<uint8_t, kCharIdentityBytes> identity{}; // whose overrides these are; zero in files before version 3

    bool Bound() const { return identity != std::array<uint8_t, kCharIdentityBytes>{}; }
    bool BelongsTo(std::span<const uint8_t> character_identity) const;
    bool Has(uint16_t field) const { return (present & field) != 0; }
    bool Any() const { return present != 0; }
    uint16_t Value(uint16_t field) const;
    void Set(uint16_t field, uint16_t value);
    void Clear(uint16_t field);
    bool operator==(const CharacterOverride& other) const = default;
};

// user/character_overrides.dat. Written atomically; an unreadable or newer file is never overwritten.
class CharacterOverrideStore {
public:
    explicit CharacterOverrideStore(std::filesystem::path path) : path_(std::move(path)) {}
    bool Load();
    bool Save() const;
    const std::filesystem::path& path() const { return path_; }
    const std::string& error() const { return error_; }

    std::optional<CharacterOverride> Get(const CustomKey& key) const;
    void Set(const CustomKey& key, std::optional<CharacterOverride> override);
    void Clear(const CustomKey& key) { Set(key, std::nullopt); }
    const std::vector<std::pair<CustomKey, CharacterOverride>>& entries() const { return entries_; }
    bool operator==(const CharacterOverrideStore& other) const { return entries_ == other.entries_; }

    std::vector<uint8_t> Serialize() const;
    bool Parse(std::span<const uint8_t> bytes);

private:
    std::filesystem::path path_;
    std::vector<std::pair<CustomKey, CharacterOverride>> entries_;
    mutable std::string error_;
};

std::filesystem::path CharacterOverrideStorePath(const std::filesystem::path& user_dir);

// Mirrors the Editor's stat fields for every character it may override: a changed field is stored and an existing entry
// is refreshed, while an untouched character without an entry stays native.
void SyncCharacterOverrides(CharacterOverrideStore& store, const SaveModel& working, const SaveModel& persisted,
                            std::string_view title_id, std::string_view save_dir);

} // namespace dcp
