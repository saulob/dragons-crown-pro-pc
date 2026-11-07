// Port-side Menu & Subtitles languages: a translation laid over the message sheets of a build's language slot when the
// game reads them (runtime_file_overlay); the archive on disk is never written. Each entry names the SHA-256 of the English
// record it was translated from; a slot in another language has a source map with the hash of its record at that index. A
// record whose installed text differs keeps that text. The tracked files are localization/<language>/sheets/<sheet>.json
// and localization/<language>/sources/<source>.json; scripts/localization/ptbr.py writes and validates them.
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
#include "game_profile.h"
#include "latin_font.h"

namespace dcp {

struct TranslationFile {
    const char* name; // SystemMsg.json
    const uint8_t* data;
    size_t size;
};

// localization/pt-BR/sheets, embedded at build time (launcher/embed_files.cmake).
std::span<const TranslationFile> PtBrTranslationFiles();
// localization/pt-BR/visual/*.dcpx.z: compressed DCPX patches against the English slot's archive entries.
std::span<const TranslationFile> PtBrVisualPatches();
// The patches of one set: localization/pt-BR/visual/base-*.dcpx.z target the base stage files, the others the USA packs.
std::vector<TranslationFile> PtBrVisualPatches(VisualSet set);
// localization/pt-BR/sources/<source>.json; nullptr when there is none.
const TranslationFile* PtBrSource(std::string_view source);

struct ArchiveTranslation {
    std::optional<ArchiveOverlay> overlay; // nothing when no record is translated or the archive cannot be read
    size_t corpus = 0;                     // records the translation holds
    size_t translated = 0;                 // records shown translated
    size_t sheets = 0;                     // sheets holding them
    size_t visuals = 0;                    // texture and layout entries replaced
    size_t refused = 0;                    // entries or records left in the slot's own text
    size_t visual_skipped = 0;             // visual entries that did not match their source
    std::vector<std::string> problems;     // why, for the log; the first few
    std::vector<std::string> visual_problems; // the visual entries left as they are, and why
    std::string error;                     // set when the archive could not be read
    bool latin_glyphs = false;             // the font was completed: the executable's glyph table must be too
    bool folded = false;                   // the font could not be completed: letters are shown without their accents
    std::vector<std::string> default_bone; // the DefaultBone records as served
    std::vector<LatinCell> latin_cells;    // the letters as the completed font holds them
    uint32_t sheets_ms = 0, visual_ms = 0, mirror_ms = 0, overlay_ms = 0; // time spent per phase
    uint32_t inflate_ms = 0, read_ms = 0;                                     // within visual_ms
};

struct TranslationRecipe {
    std::string_view suffix;                          // the slot: msgsheet/<sheet>_<suffix>.fms, msgsheet/<sheet>.fms when empty
    std::span<const TranslationFile> files;
    std::span<const TranslationFile> visual_patches = {}; // DCPX patches against the archive's own entries
    const TranslationFile* source = nullptr;          // the slot's source map; nullptr: the entries' English hashes
    std::string_view font_entry = {};                 // the font to complete with the letters, when the slot's lacks them
    std::filesystem::path mark_archive = {};          // the archive whose other/fontUTF16.ftx gives the marks
    bool glyph_table = false;                         // the executable's glyph table can take the letters
    bool french_layouts = true;                       // serve the French layouts the English slot's art was drawn for
};

ArchiveTranslation TranslateArchive(const std::filesystem::path& archive, const TranslationRecipe& recipe);
ArchiveTranslation TranslateArchive(const std::filesystem::path& archive, std::string_view suffix,
                                    std::span<const TranslationFile> files,
                                    std::span<const TranslationFile> visual_patches = {});

// The text with the accented letters of latin_glyphs replaced by their base letters.
std::string FoldLatinLetters(std::string_view text);

// The text's control sequences in order (@#(34), ##(NAME), @c(10), @/c, @@, printf conversions, $ and any lone @ or %).
// scripts/localization/ptbr.py splits text the same way.
std::vector<std::string> ControlTokens(std::string_view text);
// Same printf conversions in the same order and the same other sequences in any order. In text without a conversion a %
// is prose, and the translation may not add a conversion.
bool SameControlTokens(std::string_view source, std::string_view translation);
// The same printf conversions in the same order and the same button glyphs: what a slot in another language must share
// with the translation. Its colours and tags are its own.
bool SameArguments(std::string_view source, std::string_view translation);

} // namespace dcp
