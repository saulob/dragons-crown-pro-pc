// The real Asia/Hong Kong fonts for tests/text_tests.c: game/prepared archives read and the PT-BR letters composed with
// the launcher's own code.
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "latin_font.h"

namespace {

std::vector<dcp::LatinCell> composed_cells;
std::vector<DcpComposedGlyph> composed_table;

} // namespace

extern "C" unsigned char* test_archive_entry(const char* archive, const char* entry, size_t* size) {
    dcp::CpkArchive cpk;
    *size = 0;
    if (!cpk.Open(std::filesystem::path(DCP_SOURCE_DIR) / "game" / "prepared" / archive)) return nullptr;
    const dcp::CpkEntry* found = cpk.Find(entry);
    const std::vector<uint8_t> bytes = found ? cpk.Read(*found) : std::vector<uint8_t>();
    if (bytes.empty()) return nullptr;
    auto* copy = static_cast<unsigned char*>(std::malloc(bytes.size()));
    if (copy) std::memcpy(copy, bytes.data(), *size = bytes.size());
    return copy;
}

// Composes `font` in place; the cells stay valid until the next call.
extern "C" const DcpComposedGlyph* test_compose_latin_font(unsigned char* font, size_t size, const unsigned char* marks,
                                                           size_t marks_size, size_t* count, int* cell_pixels) {
    std::vector<uint8_t> atlas(font, font + size);
    const std::vector<uint8_t> mark_font(marks, marks + marks_size);
    std::vector<uint8_t> without_cells = atlas;
    *count = 0;
    *cell_pixels = dcp::kLatinFontCellPixels;
    if (!dcp::ComposeLatinFont(atlas, mark_font, nullptr, &composed_cells)) return nullptr;
    if (!dcp::ComposeLatinFont(without_cells, mark_font, nullptr) || without_cells != atlas) return nullptr;
    composed_table.clear();
    for (const dcp::LatinCell& cell : composed_cells) {
        composed_table.push_back({cell.code, uint8_t(cell.base), cell.alpha.data(), cell.base_alpha.data()});
    }
    std::memcpy(font, atlas.data(), size);
    *count = composed_table.size();
    return composed_table.data();
}

extern "C" int test_atlas_cell(const unsigned char* font, size_t size, int cell, unsigned char* alpha) {
    std::vector<uint8_t> pixels;
    if (!dcp::LatinFontCellAlpha(std::vector<uint8_t>(font, font + size), cell, pixels)) return 0;
    std::memcpy(alpha, pixels.data(), pixels.size());
    return 1;
}
