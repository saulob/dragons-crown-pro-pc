#include "atlas_signatures.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <vector>

#include "character_highlight_core.h"
#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "utf8.h"

namespace dcp {

namespace {

constexpr char kArchive[] = "DRACRO.CPK";

} // namespace

std::string BuildAtlasSignatures(const std::filesystem::path& game, const std::filesystem::path& out) {
    namespace hl = dcp::highlight;
    const std::filesystem::path table = out / hl::kTableFile;
    std::error_code ec;
    CpkArchive archive;
    std::string error;
    if (!std::filesystem::is_regular_file(game / kArchive, ec)) {
        std::filesystem::remove(table, ec);
        return {};
    }
    if (!archive.Open(game / kArchive, &error)) return std::string(kArchive) + ": " + error;
    std::vector<hl::AtlasEntry> entries;
    for (int klass = 0; klass < hl::kClassCount; ++klass) {
        for (int color = 0; color <= hl::kMaxColor; ++color) {
            const std::string path = hl::AtlasPath(klass, color);
            const CpkEntry* entry = archive.Find(path);
            if (!entry) continue;
            const std::vector<uint8_t> bytes = archive.Read(*entry, &error);
            if (bytes.empty()) return path + ": " + error;
            const auto atlas = hl::EntriesFor(klass, color, bytes.data(), bytes.size());
            if (!atlas) return path + " has an unexpected layout";
            entries.insert(entries.end(), atlas->begin(), atlas->end());
        }
    }
    if (entries.empty()) {
        std::filesystem::remove(table, ec);
        return {};
    }
    std::filesystem::create_directories(out, ec);
    const std::filesystem::path temp = table.string() + ".tmp";
    const std::string text = hl::FormatTable(entries);
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) return "cannot write " + temp.filename().string();
    }
    std::filesystem::rename(temp, table, ec);
    if (ec) return "cannot replace " + table.filename().string();
    return {};
}

} // namespace dcp

extern "C" int dcp_build_atlas_signatures(const char* app0, const char* image_dir, char* error, size_t error_size) {
    std::string failure;
    const std::filesystem::path out = dcp::PathFromUtf8(image_dir ? image_dir : "");
    try {
        failure = dcp::BuildAtlasSignatures(dcp::PathFromUtf8(app0 ? app0 : ""), out);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    std::error_code ec;
    if (failure.empty() && !std::filesystem::is_regular_file(out / dcp::highlight::kTableFile, ec)) {
        failure = "the game has no player atlases";
    }
    if (error && error_size) std::snprintf(error, error_size, "%s", failure.c_str());
    return failure.empty();
}
