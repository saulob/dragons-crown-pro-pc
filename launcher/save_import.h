// Import of a Dragon's Crown save into the current Pro savedata folder. The Pro, Vita and PS3 SAVE0.DAT share one
// structure, so a structurally valid file is imported directly after validation; no conversion is invented.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dcp {

struct ImportResult {
    bool valid = false;
    std::vector<std::string> errors;
    std::string platform;   // "PS4", "Vita", "PS3", or "" when unknown
    std::string region;     // "" when unknown
    std::string detected;   // one-line description for the confirmation dialog
    int data_slots = 0;
    int characters = 0;
    std::vector<uint8_t> bytes;
};

// Inspects a picked path (a SAVE0.DAT, or a folder holding one) without writing anything.
ImportResult InspectImport(const std::filesystem::path& source);

// Writes a previously inspected import over `dest_save0_path` atomically, after validating it. Returns an error string,
// empty on success.
std::optional<std::string> CommitImport(const ImportResult& import, const std::filesystem::path& dest_save0_path);

// Recognizes a Dragon's Crown PS Vita or PS3 title id; returns its platform or "" when unknown.
const char* ImportPlatformOf(std::string_view title_id);
std::string ImportRegionOf(std::string_view title_id);

} // namespace dcp
