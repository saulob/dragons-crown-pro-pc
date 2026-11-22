// Import of a SAVE0.DAT into a Pro savedata folder, byte for byte after the Pro validation; nothing is converted. A PS3
// save is unwrapped in memory and checked for the preview, but it is big-endian and no conversion to the Pro layout is
// proven, so it is refused with that reason. A Vita save is accepted only when it passes the Pro checks.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "save_zip.h"

namespace dcp {

enum class ImportSource { kSaveFile, kFolder, kZip, kRar };

enum class ImportBlock {
    kNone,
    kUnreadable,
    kTooLarge,
    kNoSave,
    kAmbiguous,
    kRarArchive,
    kArchiveInvalid,
    kArchiveUnsafe,
    kArchiveLimits,
    kArchiveEncrypted,
    kPs3NeedsPfd,
    kPs3PfdInvalid,
    kPs3UnknownTitle,
    kPs3HashMismatch,
    kPs3Undecryptable,
    kPs3UnsupportedLayout,
    kEncryptedVita,
    kInvalidSave,
};

// The English reason shown to the player; empty for kNone.
const char* ImportBlockText(ImportBlock block);

struct ImportResult {
    bool valid = false;
    ImportBlock block = ImportBlock::kNone;
    ImportSource source = ImportSource::kSaveFile;
    std::vector<std::string> errors;
    std::filesystem::path file; // the SAVE0.DAT that was read, or the archive holding it
    std::string entry;          // the path inside an archive
    std::string title_id;       // "" when none is recognized
    std::string platform;       // "PS4", "Vita", "PS3", or "" when unknown
    std::string region;         // "" when unknown
    std::string detected;       // one-line description for the confirmation dialog
    std::string format;         // what the SAVE0.DAT is, in English
    bool conversion_required = false;
    bool encrypted = false;
    std::string pfd_state;      // "Valid", "Invalid" or "Missing"; empty when no PFD is involved
    std::string decryption;     // "Supported" or "Not supported"; empty when nothing is encrypted
    bool has_param_sfo = false;
    bool has_param_pfd = false;
    uint32_t ps3_layout_version = 0; // of a PS3 save whose plaintext was read; 0 otherwise
    int candidates = 0;         // SAVE0.DAT files found in a folder or archive
    int data_slots = 0;
    int characters = 0;
    std::vector<uint8_t> bytes;
};

// Inspects a picked path (a SAVE0.DAT, a folder, or a ZIP or RAR archive) without writing anything.
ImportResult InspectImport(const std::filesystem::path& source);

// The same inspection over a ZIP already in memory; `name` is only shown.
ImportResult InspectZip(std::span<const uint8_t> zip, const std::filesystem::path& name, const ZipLimits& limits = {});

// Writes a previously inspected import over `dest_save0_path`: the destination is backed up first (under `title_id` and
// `dir_name`), then replaced atomically and read back. Returns an error string, empty on success; on failure the
// destination is left as it was.
std::optional<std::string> CommitImport(const ImportResult& import, const std::filesystem::path& dest_save0_path,
                                        const std::filesystem::path& backup_folder, std::string_view title_id,
                                        std::string_view dir_name);

// Recognizes a Dragon's Crown PS Vita or PS3 title id; returns its platform or "" when unknown.
const char* ImportPlatformOf(std::string_view title_id);
std::string ImportRegionOf(std::string_view title_id);

// Shannon entropy of the bytes, 0..8 bits per byte.
double ByteEntropy(std::span<const uint8_t> bytes);

} // namespace dcp
