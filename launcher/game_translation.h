// Port-side Menu & Subtitles languages: a translation laid over the message sheets of a regional archive's language slot
// when the game reads them (runtime_file_overlay); the archive on disk is never written. Each entry names the SHA-256 of
// the record it was translated from, so a record whose installed text differs keeps that text. The tracked files are
// localization/<language>/sheets/<sheet>.json; scripts/localization/ptbr.py writes and validates them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpk_patch.h"

namespace dcp {

struct TranslationFile {
    const char* name; // SystemMsg.json
    const uint8_t* data;
    size_t size;
};

// localization/pt-BR/sheets, embedded at build time (launcher/embed_files.cmake).
std::span<const TranslationFile> PtBrTranslationFiles();
// localization/pt-BR/visual/*.dcpx.z: compressed DCPX patches against the USA archive entries.
std::span<const TranslationFile> PtBrVisualPatches();

struct ArchiveTranslation {
    std::optional<ArchiveOverlay> overlay; // nothing when no record is translated or the archive cannot be read
    size_t translated = 0;                 // records shown translated
    size_t sheets = 0;                     // sheets holding them
    size_t visuals = 0;                    // texture and layout entries replaced
    size_t refused = 0;                    // entries or records left in the slot's own text
    size_t visual_skipped = 0;             // visual entries that did not match their source
    std::vector<std::string> problems;     // why, for the log; the first few
    std::string error;                     // set when the archive could not be read
    uint32_t sheets_ms = 0, visual_ms = 0, mirror_ms = 0, overlay_ms = 0; // time spent per phase
    uint32_t inflate_ms = 0, read_ms = 0;                                     // within visual_ms
};

// `files` laid over the `suffix` slot of the archive: msgsheet/<sheet>_<suffix>.fms. `visual_patches` are DCPX
// patches against the archive's own entries; they are applied only for the language whose files are passed.
ArchiveTranslation TranslateArchive(const std::filesystem::path& archive, std::string_view suffix,
                                    std::span<const TranslationFile> files,
                                    std::span<const TranslationFile> visual_patches = {});

// The text's control sequences in order (@#(34), ##(NAME), @c(10), @/c, @@, printf conversions, $ and any lone @ or %).
// scripts/localization/ptbr.py splits text the same way.
std::vector<std::string> ControlTokens(std::string_view text);
// Same printf conversions in the same order and the same other sequences in any order. In text without a conversion a %
// is prose, and the translation may not add a conversion.
bool SameControlTokens(std::string_view source, std::string_view translation);

} // namespace dcp
