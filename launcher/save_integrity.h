// Online save integrity (docs/ONLINE.md, Save integrity): whether a SAVE0.DAT container ever had gameplay cheats or
// gameplay Editor changes. Kept in user/online_integrity.dat; a mark is never cleared.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hp_override.h"
#include "save_data.h"
#include "save_discovery.h"

namespace dcp {

inline constexpr uint32_t kIntegrityCheatUsed = 1u << 0;
inline constexpr uint32_t kIntegrityGameplayEditorUsed = 1u << 1;
inline constexpr uint32_t kIntegrityKnownBits = kIntegrityCheatUsed | kIntegrityGameplayEditorUsed;

// One save container: the title id and the savedata directory holding SAVE0.DAT.
struct SaveKey {
    std::string title_id;
    std::string save_dir;
    bool operator==(const SaveKey&) const = default;
    auto operator<=>(const SaveKey&) const = default;
};

bool ValidSaveKey(const SaveKey& key);

// user/online_integrity.dat. Written atomically; an unreadable or newer file is never overwritten.
class SaveIntegrityStore {
public:
    explicit SaveIntegrityStore(std::filesystem::path path) : path_(std::move(path)) {}
    bool Load();
    bool Save() const;
    const std::filesystem::path& path() const { return path_; }
    const std::string& error() const { return error_; }

    uint32_t Flags(const SaveKey& key) const;
    uint32_t TitleFlags(std::string_view title_id) const;
    // Adds bits; nothing removes them.
    void Mark(const SaveKey& key, uint32_t bits);
    const std::vector<std::pair<SaveKey, uint32_t>>& entries() const { return entries_; }

    std::vector<uint8_t> Serialize() const;
    bool Parse(std::span<const uint8_t> bytes);

private:
    std::filesystem::path path_;
    std::vector<std::pair<SaveKey, uint32_t>> entries_;
    mutable std::string error_;
};

std::filesystem::path SaveIntegrityStorePath(const std::filesystem::path& user_dir);

enum class MarkResult { kAlreadySet, kMarked, kFailed };

// Reads the file again, adds the bits and writes it back, so marks another process wrote are kept.
MarkResult MarkSaveIntegrity(const std::filesystem::path& user_dir, const SaveKey& key, uint32_t bits,
                             std::string* error = nullptr);

// The one classification of a persisted Editor change: true when `after` differs from `before` anywhere except a
// present character's names and native colour (and the CRC that follows them).
bool IsGameplayEdit(const SaveModel& before, const SaveModel& after);

// kIntegrityGameplayEditorUsed when character_overrides.dat holds a stat override for a character this valid save still
// has (same slot, class and identity); 0 otherwise.
uint32_t OverrideEvidence(const CharacterOverrideStore& overrides, const DiscoveredSave& save);

// The flags of one container (save_dir given) or of every container of the title (save_dir empty), after recording what
// character_overrides.dat proves. nullopt when the state cannot be read, which is never clean.
std::optional<uint32_t> SaveIntegrityFlags(const std::filesystem::path& user_dir, std::span<const DiscoveredSave> saves,
                                           std::string_view title_id, std::string_view save_dir);

// SaveIntegrityFlags over the saves found under user_dir, as the hello's integrity_flags: -1 when unknown.
int HelloIntegrityFlags(const std::filesystem::path& user_dir, std::string_view title_id, std::string_view save_dir);

} // namespace dcp

// For the runtime. 1 when the bits are recorded.
extern "C" int dcp_save_integrity_mark(const char* user_dir, const char* title_id, const char* save_dir, unsigned bits);
// The flags (0 to 3), or -1 when unknown; a NULL or empty save_dir means every container of the title.
extern "C" int dcp_save_integrity_flags(const char* user_dir, const char* title_id, const char* save_dir);
