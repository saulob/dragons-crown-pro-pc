// The trophy key as the user supplies it: strict parsing, import from a shadPS4 keys.json or legacy config.toml, and the
// Dragon's Crown copy in <user dir>/keys.json. The key is secret local data: nothing here logs or returns its value.
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "trophies/trp_archive.h"

namespace dcp::trophies {

// Exactly 32 hexadecimal digits, either case; nothing else is accepted.
std::optional<TrophyKey> ParseKeyHex(std::string_view text);

// TrophyKeySet.ReleaseTrophyKey of a shadPS4 keys.json.
std::optional<TrophyKey> KeyFromKeysJson(std::string_view text);

// TrophyKey of the [Keys] table of an old shadPS4 config.toml.
std::optional<TrophyKey> KeyFromConfigToml(std::string_view text);

// A keys.json or config.toml chosen by the user (by extension: .toml, else JSON).
std::optional<TrophyKey> KeyFromFile(const std::filesystem::path& file);

enum class KeySource { kEntered, kImported };

// Writes only ReleaseTrophyKey into <user dir>/keys.json, keeping everything else in the file. False when the file could
// not be updated; an existing file that is not a JSON object is left alone.
bool StoreTrophyKey(const std::filesystem::path& user_dir, const TrophyKey& key, KeySource source);

// Blanks only ReleaseTrophyKey. True when no key remains.
bool ClearTrophyKey(const std::filesystem::path& user_dir);

} // namespace dcp::trophies
