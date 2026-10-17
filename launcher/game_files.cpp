#include "game_files.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <fstream>
#include <system_error>
#include <vector>

#include "crypto.h"
#include "dcp_launcher.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr std::uintmax_t kMaxSmallFile = 4u << 20;

constexpr const char* kImageFiles[] = {"boot.bin", "content.bin", "image.json"};

bool ReadSmallFile(const std::filesystem::path& path, std::string& out) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxSmallFile) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(static_cast<size_t>(size), '\0');
    in.read(out.data(), static_cast<std::streamsize>(size));
    return static_cast<std::uintmax_t>(in.gcount()) == size;
}

uint16_t Le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t Le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

struct SfoEntry {
    uint16_t format = 0;
    std::string value;
};

std::optional<SfoEntry> FindSfoEntry(const std::string& sfo, const char* key) {
    const auto* data = reinterpret_cast<const unsigned char*>(sfo.data());
    const size_t size = sfo.size();
    if (size < 20 || std::memcmp(data, "\0PSF", 4) != 0) {
        return std::nullopt;
    }
    const uint32_t keys = Le32(data + 8), values = Le32(data + 12), count = Le32(data + 16);
    for (uint32_t i = 0; i < count && 20 + static_cast<size_t>(i) * 16 + 16 <= size; ++i) {
        const unsigned char* entry = data + 20 + static_cast<size_t>(i) * 16;
        const uint16_t key_offset = Le16(entry), format = Le16(entry + 2);
        const uint32_t length = Le32(entry + 4), offset = Le32(entry + 12);
        if (static_cast<size_t>(keys) + key_offset >= size || static_cast<size_t>(values) + offset + length > size) {
            continue;
        }
        const char* name = sfo.c_str() + keys + key_offset;
        if (std::string_view(name, strnlen(name, size - keys - key_offset)) == key) {
            return SfoEntry{format, sfo.substr(values + offset, length)};
        }
    }
    return std::nullopt;
}

bool IsRegularNonEmpty(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0 && !ec;
}

GameCheck Result(GameFileState state, std::string detail, std::string sha = {}, const GameBuild* build = nullptr) {
    GameCheck check;
    check.state = state;
    check.detail = std::move(detail);
    check.eboot_sha256 = std::move(sha);
    check.build = build;
    return check;
}

std::string Shown(const std::string& value) {
    return value.empty() ? "missing" : value;
}

bool SameIdentity(const std::string& sfo, const PackageLayer& layer) {
    return SfoString(sfo, "APP_VER") == layer.app_ver && SfoString(sfo, "VERSION") == layer.sfo_version &&
           (SfoString(sfo, "CATEGORY") == "gp") == layer.update;
}

const GameBuild* BuildOfSfo(const std::string& sfo, std::span<const GameBuild> builds) {
    for (const GameBuild& build : builds) {
        const PackageLayer& finished = FinishedLayer(build);
        if (SfoString(sfo, "TITLE_ID") == build.title_id && SfoString(sfo, "CONTENT_ID") == build.content_id &&
            SfoString(sfo, "APP_VER") == finished.app_ver && SfoString(sfo, "VERSION") == finished.sfo_version) {
            return &build;
        }
    }
    return nullptr;
}

const GameBuild* BuildOfImageReport(const std::string& report, std::span<const GameBuild> builds) {
    const std::string title = JsonString(report, "title_id");
    const std::string sha = JsonString(report, "eboot_sha256");
    const std::string app_ver = JsonString(report, "app_ver");
    const GameBuild* found = nullptr;
    for (const GameBuild& build : builds) {
        if (title != build.title_id || sha != build.eboot_sha256) {
            continue;
        }
        if (!app_ver.empty()) {
            if (app_ver == FinishedLayer(build).app_ver) {
                return &build;
            }
            continue;
        }
        if (found) {
            return nullptr;
        }
        found = &build;
    }
    return found;
}

// v0.5.0 markers say version 1.03: only a tree already verified as the US build is renamed, keeping the marker's time.
bool MigrateLegacyMarker(const std::filesystem::path& marker_file, const std::string& marker, const GameBuild& build,
                         const std::string& version) {
    constexpr std::string_view kLegacy = "\"version\": \"1.03\"";
    const size_t at = marker.find(kLegacy);
    if (std::string_view(build.title_id) != "CUSA10454" || std::string_view(build.version) != "1.00" ||
        version != "1.03" || at == std::string::npos) {
        return false;
    }
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(marker_file, ec);
    std::string updated = marker;
    updated.replace(at, kLegacy.size(), "\"version\": \"" + std::string(build.version) + "\"");
    {
        std::ofstream out(marker_file, std::ios::binary | std::ios::trunc);
        out << updated;
        if (!out.flush()) {
            return false;
        }
    }
    if (!ec) {
        std::filesystem::last_write_time(marker_file, time, ec);
    }
    std::printf("Game files: %s prepared folder marker updated from version 1.03\n", build.label);
    return true;
}

} // namespace

std::string PreparationScope(const std::filesystem::path& app0) {
    std::string marker;
    if (!ReadSmallFile(app0 / kPreparedMarker, marker)) {
        return {};
    }
    return JsonString(marker, "scope");
}

std::string SfoString(const std::string& sfo, const char* key) {
    const std::optional<SfoEntry> entry = FindSfoEntry(sfo, key);
    if (!entry || (entry->format != 0x0204 && entry->format != 0x0004)) {
        return {};
    }
    return entry->value.substr(0, entry->value.find('\0'));
}

std::optional<uint32_t> SfoInteger(const std::string& sfo, const char* key, uint32_t absent) {
    const std::optional<SfoEntry> entry = FindSfoEntry(sfo, key);
    if (!entry) {
        return absent;
    }
    if (entry->format != 0x0404 || entry->value.size() != 4) {
        return std::nullopt;
    }
    return Le32(reinterpret_cast<const unsigned char*>(entry->value.data()));
}

std::string JsonString(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    at = text.find(':', at + needle.size());
    if (at == std::string::npos) {
        return {};
    }
    at = text.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || text[at] != '"') {
        return {};
    }
    const size_t end = text.find('"', at + 1);
    return end == std::string::npos ? std::string{} : text.substr(at + 1, end - at - 1);
}

std::string Sha256Hex(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    crypto::Sha256Stream stream;
    std::vector<char> block(1u << 20);
    bool ok = true;
    while (ok && in.read(block.data(), static_cast<std::streamsize>(block.size())).gcount() > 0) {
        ok = stream.Update(block.data(), static_cast<size_t>(in.gcount()));
    }
    uint8_t digest[32];
    std::string hex;
    if (ok && in.eof() && stream.Finish(digest)) {
        static const char digits[] = "0123456789abcdef";
        for (const uint8_t byte : digest) {
            hex += digits[byte >> 4];
            hex += digits[byte & 15];
        }
    }
    return hex;
}

const char* StateName(GameFileState state) {
    switch (state) {
    case GameFileState::kMissing: return "missing";
    case GameFileState::kValid: return "valid";
    case GameFileState::kDifferentVersion: return "different version";
    case GameFileState::kReadError: return "read error";
    }
    return "unknown";
}

std::string InstalledGameName(GameFileState state, const GameBuild* build) {
    if (build && state == GameFileState::kValid) {
        return std::string("PS4 ") + build->label;
    }
    return "Not installed";
}

GamePaths BuildPaths(const GameRoots& roots, const GameBuild& build) {
    return {roots.prepared / build.folder, roots.image / build.folder};
}

const GameBuild* LastPreparedBuild(const GameRoots& roots, std::span<const GameBuild> builds) {
    const GameBuild* chosen = nullptr;
    std::filesystem::file_time_type chosen_time;
    for (const GameBuild& build : builds) {
        const GamePaths paths = BuildPaths(roots, build);
        std::error_code ec;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(paths.app0 / kPreparedMarker, ec);
        if (PreparationScope(paths.app0) != "full" || ec) {
            continue;
        }
        if (!chosen || time > chosen_time) {
            chosen = &build;
            chosen_time = time;
        }
    }
    return chosen;
}

GamePaths ActiveBuildPaths(const GameRoots& roots, std::span<const GameBuild> builds) {
    const GameBuild* last = LastPreparedBuild(roots, builds);
    return BuildPaths(roots, last ? *last : builds[0]);
}

GameRoots RootsOf(const GamePaths& paths) {
    return {paths.app0.parent_path(), paths.image_dir.parent_path()};
}

GameCheck CheckSfoIdentity(const std::string& sfo, std::span<const GameBuild> builds) {
    if (const GameBuild* build = BuildOfSfo(sfo, builds)) {
        return Result(GameFileState::kValid, {}, {}, build);
    }
    const std::string title = SfoString(sfo, "TITLE_ID");
    const auto same_title = std::find_if(builds.begin(), builds.end(),
                                         [&](const GameBuild& build) { return title == build.title_id; });
    if (same_title == builds.end()) {
        return Result(GameFileState::kDifferentVersion, "param.sfo TITLE_ID is " + Shown(title));
    }
    const PackageLayer& finished = FinishedLayer(*same_title);
    const struct {
        const char* key;
        const char* wanted;
    } identity[] = {
        {"CONTENT_ID", same_title->content_id},
        {"APP_VER", finished.app_ver},
        {"VERSION", finished.sfo_version},
    };
    for (const auto& item : identity) {
        const std::string found = SfoString(sfo, item.key);
        if (found != item.wanted) {
            return Result(GameFileState::kDifferentVersion, std::string("param.sfo ") + item.key + " is " + Shown(found));
        }
    }
    return Result(GameFileState::kDifferentVersion, "param.sfo matches no supported build");
}

GameCheck CheckPackageIdentity(const std::string& sfo, std::span<const GameBuild> builds) {
    const std::string title = SfoString(sfo, "TITLE_ID");
    const auto same_title = std::find_if(builds.begin(), builds.end(),
                                         [&](const GameBuild& build) { return title == build.title_id; });
    if (same_title == builds.end()) {
        return Result(GameFileState::kDifferentVersion, "param.sfo TITLE_ID is " + Shown(title));
    }
    const std::string content = SfoString(sfo, "CONTENT_ID");
    if (content != same_title->content_id) {
        return Result(GameFileState::kDifferentVersion, "param.sfo CONTENT_ID is " + Shown(content));
    }
    for (const GameBuild& build : builds) {
        if (title == build.title_id && SameIdentity(sfo, FinishedLayer(build))) {
            GameCheck check = Result(GameFileState::kValid, {}, {}, &build);
            check.layer = build.layers.size() - 1;
            return check;
        }
    }
    return Result(GameFileState::kDifferentVersion,
                  "param.sfo APP_VER " + Shown(SfoString(sfo, "APP_VER")) + " / VERSION " +
                      Shown(SfoString(sfo, "VERSION")) + " is not part of a supported build",
                  {}, &*same_title);
}

GameCheck CheckGameTree(const std::filesystem::path& app0, std::span<const GameBuild> builds) {
    std::error_code ec;
    if (!std::filesystem::is_directory(app0, ec)) {
        return Result(GameFileState::kMissing, "game folder not found");
    }
    std::string sfo;
    if (!ReadSmallFile(app0 / "sce_sys" / "param.sfo", sfo)) {
        return Result(std::filesystem::exists(app0 / "sce_sys" / "param.sfo", ec) ? GameFileState::kReadError
                                                                                  : GameFileState::kMissing,
                      "cannot read sce_sys/param.sfo");
    }
    const GameCheck identity = CheckSfoIdentity(sfo, builds);
    if (identity.state != GameFileState::kValid) {
        return identity;
    }
    const GameBuild& build = *identity.build;

    for (const char* name : kSharedGameFiles) {
        if (!IsRegularNonEmpty(app0 / name)) {
            return Result(GameFileState::kMissing, std::string("missing ") + name, {}, &build);
        }
    }
    if (build.regional_archive && !IsRegularNonEmpty(app0 / build.regional_archive)) {
        return Result(GameFileState::kMissing, std::string("missing ") + build.regional_archive, {}, &build);
    }
    for (const char* name : build.extra_files) {
        if (!IsRegularNonEmpty(app0 / name)) {
            return Result(GameFileState::kMissing, std::string("missing ") + name, {}, &build);
        }
    }

    const std::string sha = Sha256Hex(app0 / "eboot.bin");
    if (sha.empty()) {
        return Result(GameFileState::kReadError, "cannot read eboot.bin", {}, &build);
    }
    if (sha != build.eboot_sha256) {
        return Result(GameFileState::kDifferentVersion, "eboot.bin is not the supported build", sha, &build);
    }
    return Result(GameFileState::kValid, {}, sha, &build);
}

GameCheck CheckGameFiles(const GamePaths& paths, std::span<const GameBuild> builds) {
    std::error_code ec;
    if (!std::filesystem::is_directory(paths.app0, ec)) {
        return Result(GameFileState::kMissing, "game folder not found");
    }

    const std::filesystem::path marker_file = paths.app0 / kPreparedMarker;
    if (!std::filesystem::exists(marker_file, ec)) {
        return Result(GameFileState::kMissing, "game folder has no preparation marker");
    }
    std::string marker;
    if (!ReadSmallFile(marker_file, marker)) {
        return Result(GameFileState::kReadError, "cannot read the preparation marker");
    }
    const std::string version = JsonString(marker, "version");
    const std::string scope = JsonString(marker, "scope");
    if (version.empty() || scope.empty()) {
        return Result(GameFileState::kMissing, "game preparation did not finish");
    }
    if (scope != "full") {
        return Result(GameFileState::kMissing, "only the " + scope + " files were prepared");
    }

    const GameCheck tree = CheckGameTree(paths.app0, builds);
    if (tree.state != GameFileState::kValid) {
        return tree;
    }
    const GameBuild& build = *tree.build;
    const std::string& sha = tree.eboot_sha256;
    if (version != build.version && !MigrateLegacyMarker(marker_file, marker, build, version)) {
        return Result(GameFileState::kDifferentVersion, "prepared version " + version, sha, &build);
    }
    for (const char* name : kImageFiles) {
        if (!IsRegularNonEmpty(paths.image_dir / name)) {
            return Result(GameFileState::kMissing, std::string("boot image: missing ") + name, sha, &build);
        }
    }
    std::string report;
    if (!ReadSmallFile(paths.image_dir / "image.json", report)) {
        return Result(GameFileState::kReadError, "cannot read the boot image report", sha, &build);
    }
    if (BuildOfImageReport(report, builds) != &build) {
        return Result(GameFileState::kDifferentVersion, "the boot image was built from another eboot.bin", sha, &build);
    }
    return Result(GameFileState::kValid, {}, sha, &build);
}

} // namespace dcp

// The supported build a boot image folder was built from, for the loader; 0 when it is not one.
extern "C" int dcp_game_build(const char* image_dir, DcpGameBuild* out) {
    std::string report;
    if (!image_dir || !out || !dcp::ReadSmallFile(dcp::PathFromUtf8(image_dir) / "image.json", report)) {
        return 0;
    }
    const dcp::GameBuild* build = dcp::BuildOfImageReport(report, dcp::SupportedBuilds());
    if (!build) {
        return 0;
    }
    out->label = build->label;
    out->cheats = build->cheats;
    out->resolution_sites = build->resolution_sites.data();
    out->resolution_site_count = build->resolution_sites.size();
    return 1;
}
