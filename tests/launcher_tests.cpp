// Focused tests for the launcher's logic: game file validation, the settings file, the Play gate, the window
// placement, the Display options and the Controller prompts patch. They use synthetic files in a temporary folder; no
// window is opened.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "controller_prompts.h"
#include "game_patches.h"
#include "cpk_patch.h"
#include "dcp_cheats.h"
#include "dcp_launcher.h"
#include "startup_phase.h"
#include "dcp_graphics.h"
#include "dcp_keys.h"
#include "dcp_overlay_box.h"
#include "dcp_ping.h"
#include "dcp_png.h"
#include "dcp_settings.h"
#include "dcp_title.h"
#include "default_message_hook.h"
#include "exit_game_text.h"
#include "file_browser.h"
#include "fms_sheet.h"
#include "game_files.h"
#include "game_install.h"
#include "game_process.h"
#include "net/connection_test.h"
#include "npc_voice_resources.h"
#include "game_translation.h"
#include "header_art.h"
#include "localization/ui_text.h"
#include <zlib.h>
#include "net/identity.h"
#include "net/online_version.h"
#include "online_client.h"
#include "online_settings.h"
#include "online_view.h"
#include "options.h"
#include "page_icons.h"
#include "paths.h"
#include "placement.h"
#include "play_gate.h"
#include "runtime_log.h"
#include "settings.h"
#include "cyrillic_font.h"
#include "latin_font.h"
#include "latin_glyphs.h"
#include "text_languages.h"
#include "trg/layout.h"
#include "trg/page_navigation.h"
#include "trg/platform.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// tools/test.ps1 reads these: a group with no line, or a failed one, fails the launcher gate.
std::vector<std::pair<std::string, int>> g_groups;

void InGroup(const char* name, void (*test)()) {
    const int before = g_failures;
    try {
        test();
    } catch (const std::exception& error) {
        ++g_failures;
        std::printf("FAIL group %s: exception: %s\n", name, error.what());
    }
    for (auto& group : g_groups) {
        if (group.first == name) {
            group.second += g_failures - before;
            return;
        }
    }
    g_groups.emplace_back(name, g_failures - before);
}

bool ValidUtf8Text(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 4;
        if (extra == 4 || i + extra >= text.size()) {
            return false;
        }
        for (size_t j = 1; j <= extra; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void WriteAll(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// A param.sfo with the given string entries.
std::string MakeSfo(const std::vector<std::pair<std::string, std::string>>& items) {
    std::string keys, values;
    std::string entries;
    auto le16 = [](std::string& out, unsigned v) { out += char(v & 255); out += char(v >> 8 & 255); };
    auto le32 = [](std::string& out, unsigned v) {
        for (int i = 0; i < 4; ++i) out += char(v >> (8 * i) & 255);
    };
    for (const auto& [key, value] : items) {
        le16(entries, unsigned(keys.size()));
        le16(entries, 0x0204);
        le32(entries, unsigned(value.size() + 1));
        le32(entries, unsigned(value.size() + 1));
        le32(entries, unsigned(values.size()));
        keys += key + '\0';
        values += value + '\0';
    }
    while (keys.size() % 4) keys += '\0';
    std::string sfo(1, '\0');
    sfo += "PSF";
    le32(sfo, 0x101);
    le32(sfo, unsigned(20 + entries.size()));
    le32(sfo, unsigned(20 + entries.size() + keys.size()));
    le32(sfo, unsigned(items.size()));
    return sfo + entries + keys + values;
}

const GameBuild& kUs = SupportedBuilds()[0];
const GameBuild& kUs101 = SupportedBuilds()[1];
const GameBuild& kJp100 = SupportedBuilds()[2];
const GameBuild& kJp102 = SupportedBuilds()[3];
const GameBuild& kJp103 = SupportedBuilds()[4];
const GameBuild& kCk100 = SupportedBuilds()[5];
const GameBuild& kCk = SupportedBuilds()[6];
const GameBuild& kEu = SupportedBuilds()[7];

constexpr char kEboot[] = "synthetic eboot bytes";

struct Fixture {
    fs::path root;
    GamePaths paths;
    GameBuild build;
    std::vector<GameBuild> builds;
    std::string eboot_sha;

    explicit Fixture(const fs::path& base, const char* name, const GameBuild& original = kUs)
        : root(base / name), build(original) {
        fs::remove_all(root);
        paths.app0 = root / "app0";
        paths.image_dir = root / "image";
        WriteAll(paths.app0 / "eboot.bin", kEboot);
        eboot_sha = Sha256Hex(paths.app0 / "eboot.bin");
        build.eboot_sha256 = eboot_sha.c_str();
        builds = {build};
        Rebuild();
    }

    // The complete, valid layout of this build.
    void Rebuild() {
        WriteAll(paths.app0 / ".dcp_view.json", "{\n  \"version\": \"" + std::string(build.version) + "\",\n  \"scope\": \"full\"\n}");
        WriteAll(paths.app0 / "sce_sys" / "param.sfo",
                 MakeSfo({{"TITLE", "Dragon's Crown Pro"},
                          {"TITLE_ID", std::string(build.title_id)},
                          {"CONTENT_ID", std::string(build.content_id)},
                          {"APP_VER", std::string(FinishedLayer(build).app_ver)},
                          {"VERSION", std::string(FinishedLayer(build).sfo_version)}}));
        for (const char* name : {"sce_discmap.plt", "DRACRO.CPK", "sce_module/libc.prx", "sce_module/libSceFios2.prx",
                                 "sce_module/libSceNpToolkit2.prx"}) {
            WriteAll(paths.app0 / name, "data");
        }
        if (build.regional_archive) {
            WriteAll(paths.app0 / build.regional_archive, "data");
        }
        for (const char* name : build.extra_files) {
            WriteAll(paths.app0 / name, "data");
        }
        WriteAll(paths.image_dir / "boot.bin", "boot");
        WriteAll(paths.image_dir / "content.bin", "content");
        WriteAll(paths.image_dir / "image.json",
                 "{\n  \"title_id\": \"" + std::string(build.title_id) + "\",\n  \"eboot_sha256\": \"" + eboot_sha + "\"\n}");
    }

    GameFileState State() const { return CheckGameFiles(paths, builds).state; }
};

void TestSha256AndJson(const fs::path& base) {
    WriteAll(base / "abc.txt", "abc");
    CHECK(Sha256Hex(base / "abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    WriteAll(base / "empty.txt", "");
    CHECK(Sha256Hex(base / "empty.txt") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex(base / "does-not-exist") .empty());

    const std::string json = "{\n  \"version\": \"1.03\",\n  \"scope\": \"full\",\n  \"layers\": [\"x\"]\n}";
    CHECK(JsonString(json, "version") == "1.03");
    CHECK(JsonString(json, "scope") == "full");
    CHECK(JsonString(json, "layers").empty());
    CHECK(JsonString(json, "missing").empty());
}

void TestGameFiles(const fs::path& base) {
    {
        Fixture f(base, "valid");
        const GameCheck check = CheckGameFiles(f.paths, f.builds);
        CHECK(check.state == GameFileState::kValid);
        CHECK(check.eboot_sha256 == f.eboot_sha);
    }
    {
        Fixture f(base, "no-folder");
        fs::remove_all(f.paths.app0);
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "no-marker");
        fs::remove(f.paths.app0 / ".dcp_view.json");
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "unfinished-marker");
        WriteAll(f.paths.app0 / ".dcp_view.json", "{\n  \"description\": \"CUSA10454 1.03\"\n}");
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "core-scope");
        WriteAll(f.paths.app0 / ".dcp_view.json", "{\"version\": \"1.03\", \"scope\": \"core\"}");
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "other-version");
        WriteAll(f.paths.app0 / ".dcp_view.json", "{\"version\": \"1.04\", \"scope\": \"full\"}");
        CHECK(f.State() == GameFileState::kDifferentVersion);
    }
    {
        Fixture f(base, "other-title");
        WriteAll(f.paths.app0 / "sce_sys" / "param.sfo",
                 MakeSfo({{"TITLE_ID", "CUSA00000"},
                          {"CONTENT_ID", std::string(kUs.content_id)},
                          {"APP_VER", std::string(FinishedLayer(kUs).app_ver)},
                          {"VERSION", std::string(FinishedLayer(kUs).sfo_version)}}));
        CHECK(f.State() == GameFileState::kDifferentVersion);
    }
    {
        Fixture f(base, "other-sfo-version");
        WriteAll(f.paths.app0 / "sce_sys" / "param.sfo",
                 MakeSfo({{"TITLE_ID", std::string(kUs.title_id)},
                          {"CONTENT_ID", std::string(kUs.content_id)},
                          {"APP_VER", std::string(FinishedLayer(kUs).app_ver)},
                          {"VERSION", "01.00"}}));
        CHECK(f.State() == GameFileState::kDifferentVersion);
    }
    {
        Fixture f(base, "no-sfo");
        fs::remove(f.paths.app0 / "sce_sys" / "param.sfo");
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "no-archive");
        fs::remove(f.paths.app0 / "DRACRO.CPK");
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "empty-archive");
        WriteAll(f.paths.app0 / "DRACRO_US.CPK", "");
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "other-eboot");
        WriteAll(f.paths.app0 / "eboot.bin", "a different build");
        CHECK(f.State() == GameFileState::kDifferentVersion);
    }
    {
        Fixture f(base, "no-image");
        fs::remove_all(f.paths.image_dir);
        CHECK(f.State() == GameFileState::kMissing);
    }
    {
        Fixture f(base, "stale-image");
        WriteAll(f.paths.image_dir / "image.json",
                 "{\"title_id\": \"" + std::string(kUs.title_id) + "\", \"eboot_sha256\": \"" + std::string(64, '0') + "\"}");
        CHECK(f.State() == GameFileState::kDifferentVersion);
    }
    {
        // The supported builds reject the synthetic eboot.
        Fixture f(base, "supported-builds");
        CHECK(CheckGameFiles(f.paths).state == GameFileState::kDifferentVersion);
    }
}

// The C++ builds must not drift from the profiles the Python tools use.
void TestFingerprintMatchesProfile() {
    const std::string profile = ReadAll(fs::path(DCP_SOURCE_DIR) / "scripts" / "game_profiles.py");
    CHECK(!profile.empty());
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(profile.find("'" + std::string(build.title_id) + "': {") != std::string::npos);
        CHECK(profile.find("'" + std::string(build.version) + "': {") != std::string::npos);
        CHECK(profile.find("'content_id': '" + std::string(build.content_id) + "'") != std::string::npos);
        CHECK(profile.find("'app_ver': '" + std::string(FinishedLayer(build).app_ver) + "'") != std::string::npos);
        CHECK(profile.find("'sfo_version': '" + std::string(FinishedLayer(build).sfo_version) + "'") != std::string::npos);
        CHECK(profile.find("'eboot_sha256': '" + std::string(build.eboot_sha256) + "'") != std::string::npos);
        CHECK(profile.find("'folder': '" + std::string(build.folder) + "'") != std::string::npos);
        const std::string archive = build.regional_archive ? "'" + std::string(build.regional_archive) + "'" : "None";
        CHECK(profile.find("'regional_archive': " + archive) != std::string::npos);
        std::string extra = "(";
        for (const char* name : build.extra_files) {
            extra += std::string("'") + name + "',";
        }
        CHECK(profile.find("'extra_files': " + extra + ")") != std::string::npos);
        CHECK(std::string(build.eboot_sha256).size() == 64);
        CHECK(profile.find("'region': '" + std::string(build.region) + "'") != std::string::npos);
        CHECK(std::string(build.label) == std::string(build.title_id) + " " + build.version + " " + build.region);
        for (const PackageLayer& layer : build.layers) {
            CHECK(profile.find("'app_ver': '" + std::string(layer.app_ver) + "', 'sfo_version': '" +
                               std::string(layer.sfo_version) + "', 'update': " + (layer.update ? "True" : "False") + "}") !=
                  std::string::npos);
        }
    }
    CHECK(SupportedBuildsText() ==
          "CUSA10454 1.00 USA, CUSA10454 1.01 USA, CUSA07001 1.00 Japan, CUSA07001 1.02 Japan, CUSA07001 1.03 Japan, CUSA10209 1.00 Asia/Hong Kong, CUSA10209 1.02 Asia/Hong Kong and CUSA10487 1.03 Europe");
    CHECK(std::string(kUs.folder) != kCk.folder);
    std::string modules = "'modules': (";
    for (const char* module : kBundledModules) {
        modules += std::string(module == kBundledModules[0] ? "" : ", ") + "'" + module + "'";
    }
    CHECK(profile.find(modules + ")") != std::string::npos);
}

void TestSettings(const fs::path& base) {
    const fs::path file = base / "settings" / "dcp.ini";
    fs::remove_all(file.parent_path());
    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(kShowLauncherKey, true));
        CHECK(settings.GetBool(kVSyncKey, true));
        CHECK(settings.GetInt(kWindowWidthKey, kDefaultWindowWidth) == kDefaultWindowWidth);
        CHECK(settings.Save()); // nothing pending: no file is needed
    }

    // A file another build wrote: "key=value" lines, keys the launcher does not manage, a comment.
    WriteAll(file,
             "# video core settings\n"
             "upscaler=off\n"
             "show_fps=0\n"
             "vsync=1\n"
             "display_mode=fullscreen\n"
             "overlay_fps=1\n"
             "async_shaders=0\n");
    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(kVSyncKey, false));
        CHECK(settings.Get(kDisplayModeKey, "windowed") == "windowed"); // not a mode the game accepts: the default

        settings.SetBool(kVSyncKey, false);
        settings.SetInt(kWindowWidthKey, 2560);
        settings.SetInt(kWindowHeightKey, 1440);
        settings.SetInt(kMonitorKey, 2);
        settings.SetBool(kShowFpsTitleKey, true);
        settings.SetBool(kShowResolutionTitleKey, true);
        const std::string before_save = ReadAll(file);
        CHECK(before_save.find("vsync=1") != std::string::npos); // pending until Save
        CHECK(settings.Save());
    }
    {
        const std::string text = ReadAll(file);
        CHECK(text.find("upscaler=off") != std::string::npos);      // unrelated lines survive
        CHECK(text.find("overlay_fps=1") != std::string::npos);
        CHECK(text.find("async_shaders=0") != std::string::npos);
        CHECK(text.find("display_mode=fullscreen") != std::string::npos); // untouched: not changed in the launcher
        CHECK(text.find("# video core settings") != std::string::npos);
        CHECK(text.find("vsync = 0") != std::string::npos || text.find("vsync=0") != std::string::npos);
        LauncherSettings reread(file);
        CHECK(!reread.GetBool(kVSyncKey, true));
        CHECK(reread.GetInt(kWindowWidthKey, 0) == 2560);
        CHECK(reread.GetInt(kWindowHeightKey, 0) == 1440);
        CHECK(reread.GetInt(kMonitorKey, 0) == 2);
        CHECK(reread.GetBool(kShowFpsTitleKey, false));
        CHECK(reread.GetBool(kShowResolutionTitleKey, false));
        CHECK(reread.Get(kDisplayModeKey, "windowed") == "windowed");
    }

    // Booleans are written as 1/0.
    {
        LauncherSettings settings(file);
        settings.SetBool(kSharpenKey, false);
        CHECK(settings.Get(kSharpenKey) == "0");
        settings.SetBool(kSharpenKey, true);
        CHECK(settings.Get(kSharpenKey) == "1");
    }

    // SaveNow (the window placement) leaves other pending edits pending.
    {
        LauncherSettings settings(file);
        settings.SetBool(kUpscaleKey, true);
        WindowPlacement placement;
        placement.normal = {100, 80, 1380, 800};
        placement.maximized = true;
        CHECK(settings.SaveNow(PlacementValues(placement)));
        CHECK(ReadAll(file).find("fsr1") == std::string::npos);
        CHECK(settings.GetBool(kUpscaleKey, false)); // still pending in memory
        LauncherSettings reread(file);
        const WindowPlacement stored = ReadPlacement(reread);
        CHECK(stored == placement);
        CHECK(!reread.GetBool(kUpscaleKey, false));
    }

    // Reset restores the defaults but keeps the launcher's own window placement.
    {
        LauncherSettings settings(file);
        settings.SetBool(kShowLauncherKey, false);
        settings.SetInt(kWindowWidthKey, 3840);
        settings.ResetToDefaults();
        CHECK(settings.GetBool(kShowLauncherKey, false));
        CHECK(settings.GetInt(kWindowWidthKey, 0) == kDefaultWindowWidth);
        CHECK(settings.GetInt(kLauncherWidthKey, 0) == 1280);
        CHECK(settings.Save());
        LauncherSettings reread(file);
        CHECK(reread.GetBool(kShowLauncherKey, false));
        CHECK(reread.GetInt(kLauncherWidthKey, 0) == 1280);
        CHECK(ReadAll(file).find("overlay_fps=1") != std::string::npos);
    }

    // A file that cannot be written reports it and keeps the edits pending.
    {
        LauncherSettings settings(base / "settings" / "dcp.ini" / "not-a-folder" / "dcp.ini");
        settings.SetBool(kShowLauncherKey, false);
        CHECK(!settings.Save());
        CHECK(!settings.GetBool(kShowLauncherKey, true));
    }
}

void TestPaths() {
    const PathRequest none;
    const fs::path version = "v1.03";

    const fs::path packaged = "X:/DragonsCrownPro";
    Paths p = ResolvePaths(none, packaged);
    CHECK(p.app0 == packaged / "game" / "prepared" / version);
    CHECK(p.image_dir == packaged / "game" / "image" / version);
    CHECK(p.boot == p.image_dir / "boot.bin");
    CHECK(p.content_profile == p.image_dir / "content.bin");
    CHECK(p.user_dir == packaged / "user");
    CHECK(p.logs_dir == packaged / "logs");

    p = ResolvePaths(none, "S:/repo/build/cmake/clang64/bin");
    CHECK(p.app0 == fs::path("S:/repo/game/prepared") / version);
    CHECK(p.image_dir == fs::path("S:/repo/build/image") / version);
    CHECK(p.user_dir == fs::path("S:/repo/user"));
    CHECK(p.logs_dir == fs::path("S:/repo/build/logs"));

    p = ResolvePaths(none, "X:/Apps/bin"); // a bin folder outside a build tree is just a packaged folder
    CHECK(p.app0 == fs::path("X:/Apps/bin/game/prepared") / version);
    p = ResolvePaths(none, "S:/repo/build/cmake/clang64/other");
    CHECK(p.user_dir == fs::path("S:/repo/build/cmake/clang64/other/user"));

    const PathRequest all{"D:/img/boot.bin", "D:/content/content.bin", "D:/game", "D:/user", "D:/logs"};
    p = ResolvePaths(all, packaged);
    CHECK(p.boot == fs::path("D:/img/boot.bin") && p.image_dir == fs::path("D:/img"));
    CHECK(p.content_profile == fs::path("D:/content/content.bin"));
    CHECK(p.app0 == fs::path("D:/game") && p.user_dir == fs::path("D:/user") && p.logs_dir == fs::path("D:/logs"));

    PathRequest boot_only;
    boot_only.boot = "D:/img/boot.bin";
    p = ResolvePaths(boot_only, packaged);
    CHECK(p.content_profile == fs::path("D:/img/content.bin")); // next to the boot file
    CHECK(p.app0 == packaged / "game" / "prepared" / version);

    PathRequest bare;
    bare.boot = "boot.bin";
    CHECK(ResolvePaths(bare, packaged).image_dir == fs::path("."));

    PathRequest user_only;
    user_only.user_dir = "D:/user";
    p = ResolvePaths(user_only, packaged);
    CHECK(p.user_dir == fs::path("D:/user") && p.logs_dir == packaged / "logs");

    // The C entry point finds this test executable in the build tree: the repository's folders.
    DcpPaths found;
    const DcpPathRequest nothing{};
    CHECK(dcp_resolve_paths(&nothing, &found) == 1);
    const fs::path repository = DCP_SOURCE_DIR;
    CHECK(fs::weakly_canonical(PathFromUtf8(found.user_dir)) == fs::weakly_canonical(repository / "user"));
    const GamePaths active = ActiveBuildPaths({repository / "game" / "prepared", repository / "build" / "image"});
    CHECK(fs::weakly_canonical(PathFromUtf8(found.app0)) == fs::weakly_canonical(active.app0));
    CHECK(fs::weakly_canonical(PathFromUtf8(found.boot)) == fs::weakly_canonical(active.image_dir / "boot.bin"));
    CHECK(fs::weakly_canonical(PathFromUtf8(found.logs_dir)) == fs::weakly_canonical(repository / "build" / "logs"));
    const DcpPathRequest explicit_user{nullptr, nullptr, nullptr, "D:/elsewhere", nullptr};
    CHECK(dcp_resolve_paths(&explicit_user, &found) == 1 && std::string(found.user_dir) == "D:/elsewhere");

    // The log folder follows the executable, never the current working directory.
    const fs::path cwd = fs::current_path();
    std::error_code ec;
    fs::current_path(fs::temp_directory_path(), ec);
    if (!ec) {
        DcpPaths from_elsewhere;
        CHECK(dcp_resolve_paths(&nothing, &from_elsewhere) == 1);
        CHECK(fs::path(from_elsewhere.logs_dir) == fs::path(found.logs_dir));
        fs::current_path(cwd, ec);
        CHECK(!ec);
    }
}

// Creates a log the way the loader does and writes some text into it; the path, or empty on failure.
std::string CreateLog(const fs::path& dir, const char* file = nullptr, const std::string& text = {}) {
    char path[4096];
    void* handle = nullptr;
    const std::string folder = dir.string();
    if (!runtime_log_create(folder.c_str(), file, path, sizeof(path), &handle)) {
        return {};
    }
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(handle, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(handle);
#else
    const int fd = int(reinterpret_cast<intptr_t>(handle));
    const ssize_t written = write(fd, text.data(), text.size());
    (void)written;
    close(fd);
#endif
    return path;
}

void TestLogNumbering(const fs::path& base) {
    const fs::path dir = base / "numbered" / "logs";
    const std::string first = CreateLog(dir, nullptr, "one");
    CHECK(fs::path(first).filename() == "dragons_crown_pro_001.log");
    char folder[4096];
    CHECK(dcp_logs_directory(dir.string().c_str(), first.c_str(), folder, sizeof(folder)) == 1);
    CHECK(fs::path(folder) == dir); // the launcher's logs folder is the loader's log directory
    const std::string second = CreateLog(dir, nullptr, "two");
    CHECK(fs::path(second).filename() == "dragons_crown_pro_002.log");
    CHECK(ReadAll(first) == "one" && ReadAll(second) == "two"); // an earlier log is never overwritten

    const fs::path busy = base / "busy";
    WriteAll(busy / "dragons_crown_pro_007.log", "seven");
    WriteAll(busy / "dragons_crown_pro_abc.log", "x");
    WriteAll(busy / "dragons_crown_pro_5.log.bak", "x");
    WriteAll(busy / "other_009.log", "x");
    const std::string next = CreateLog(busy, nullptr, "eight");
    CHECK(fs::path(next).filename() == "dragons_crown_pro_008.log");
    CHECK(ReadAll(busy / "dragons_crown_pro_007.log") == "seven");
    CHECK(fs::path(CreateLog(busy)).filename() == "dragons_crown_pro_009.log");
    WriteAll(busy / "dragons_crown_pro_1000.log", "x");
    CHECK(fs::path(CreateLog(busy)).filename() == "dragons_crown_pro_1001.log");

    const fs::path explicit_file = base / "explicit" / "deeper" / "my.log";
    CHECK(CreateLog(base / "ignored", explicit_file.string().c_str(), "mine") == explicit_file.string());
    CHECK(ReadAll(explicit_file) == "mine" && !fs::exists(base / "ignored"));
}

void TestDefaultLogFolders(const fs::path& base) {
    const PathRequest none;
    const fs::path packaged = base / "packaged";
    fs::create_directories(packaged);
    const std::string packaged_log = CreateLog(ResolvePaths(none, packaged).logs_dir);
    CHECK(fs::path(packaged_log) == packaged / "logs" / "dragons_crown_pro_001.log");
    CHECK(fs::path(CreateLog(ResolvePaths(none, packaged).logs_dir)) == packaged / "logs" / "dragons_crown_pro_002.log");

    const fs::path bin = base / "repo" / "build" / "cmake" / "clang64" / "bin";
    fs::create_directories(bin);
    CHECK(fs::path(CreateLog(ResolvePaths(none, bin).logs_dir)) == base / "repo" / "build" / "logs" / "dragons_crown_pro_001.log");

    PathRequest asked;
    asked.logs_dir = (base / "custom").string();
    CHECK(fs::path(CreateLog(ResolvePaths(asked, packaged).logs_dir)) == base / "custom" / "dragons_crown_pro_001.log");
}

// Last: it redirects this process's stdout and stderr.
void TestLogRedirect(const fs::path& base) {
    char path[4096];
    const fs::path capture = base / "tee" / "test_capture.log";
    CHECK(runtime_log_start((base / "tee").string().c_str(), capture.string().c_str(), path, sizeof(path)) == 1);
    std::printf("stdout marker\n");
    std::fflush(stdout);
    std::fprintf(stderr, "stderr marker\n");
    runtime_log_flush();
    const std::string text = ReadAll(path);
    CHECK(fs::path(path) == capture); // a test capture, never a dragons_crown_pro_*.log
    CHECK(text.find("stdout marker\nstderr marker\n") != std::string::npos);
}

// Every environment variable the loader, the runtime, the launcher and the video core read is one of this
// project's DCP_* names, and none of them can bring back an inherited setting that changes the rendering.
void TestEnvironmentVariables() {
    const fs::path source(DCP_SOURCE_DIR);
    // Unix/macOS branches of vendored shadPS4 code, the Linux AppImage's own path and desktop session, and the
    // Gamescope session (gpu/shim/dcp_present.h, never read on Windows).
    const std::vector<std::string> platform = {"HOME",     "XDG_DATA_HOME", "VK_DRIVER_FILES", "APPIMAGE",
                                               "USER",     "DISPLAY",       "GAMESCOPE_WAYLAND_DISPLAY"};
    std::vector<std::string> names, inherited;
    int files = 0;
    for (const char* dir : {"src", "launcher", "gpu/shim", "gpu/shadps4"}) {
        for (const auto& entry : fs::recursive_directory_iterator(source / dir)) {
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file() ||
                (ext != ".c" && ext != ".cpp" && ext != ".h" && ext != ".hpp")) {
                continue;
            }
            ++files;
            const std::string text = ReadAll(entry.path());
            if (text.find("GOW3_") != std::string::npos) {
                inherited.push_back(entry.path().string());
            }
            for (const char* call : {"getenv(\"", "Flag(\""}) {
                for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1)) {
                    const size_t begin = at + std::strlen(call);
                    names.push_back(text.substr(begin, text.find('"', begin) - begin));
                }
            }
        }
    }
    CHECK(files > 400);
    CHECK(std::find(names.begin(), names.end(), "DCP_FRAME_STATS") != names.end()); // the scan sees the reads
    for (const std::string& path : inherited) {
        std::printf("GOW3_ name in %s\n", path.c_str());
    }
    CHECK(inherited.empty());
    std::vector<std::string> unexpected;
    for (const std::string& name : names) {
        bool ok = name.rfind("DCP_", 0) == 0 || std::find(platform.begin(), platform.end(), name) != platform.end();
        for (const char* forbidden : {"ASYNC", "WORKER", "FRAMES", "FPS", "VBLANK", "UPSCAL", "READBACK", "CONFIG",
                                      "GPU_ID", "PRESENT", "RECORD_THREAD", "DRAW_PIPE"}) {
            ok = ok && name.find(forbidden) == std::string::npos;
        }
        if (!ok) {
            std::printf("unexpected environment variable %s\n", name.c_str());
            unexpected.push_back(name);
        }
    }
    CHECK(unexpected.empty());
}

// The fixes that made the game correct are code, not settings.
void TestCorrectnessStaysFixed() {
    const fs::path source(DCP_SOURCE_DIR);
    const fs::path renderer = source / "gpu" / "shadps4" / "video_core" / "renderer_vulkan";
    CHECK(ReadAll(renderer / "vk_pipeline_cache.cpp")
              .find("bool PipelineCache::AsyncCompilation() const {\n    return false;\n}") != std::string::npos);
    CHECK(ReadAll(renderer / "vk_draw_prep.cpp").find("constexpr u32 WorkerCount = 0;") != std::string::npos);
    const std::string presenter = ReadAll(renderer / "vk_presenter.cpp");
    CHECK(presenter.find("draw_scheduler.Wait(previous_frame_tick);") != std::string::npos);
    // The redrawn last frame leaves the free queue while it is shown; the swapchain waits for the device only
    // under the queue submission lock.
    CHECK(presenter.find("free_queue.erase(it);") != std::string::npos);
    const std::string swapchain = ReadAll(renderer / "vk_swapchain.cpp");
    const size_t wait = swapchain.find("waitIdle()");
    CHECK(wait != std::string::npos && swapchain.find("waitIdle()", wait + 1) == std::string::npos);
    CHECK(swapchain.rfind("std::scoped_lock lock{Scheduler::submit_mutex};", wait) != std::string::npos);
    const std::string emulator = ReadAll(source / "gpu" / "shim" / "core" / "emulator_settings.h");
    CHECK(emulator.find("u32 GetFrameLimit() { return 0; }") != std::string::npos);
    CHECK(emulator.find("u32 GetVblankFrequency() { return 60; }") != std::string::npos);
    CHECK(emulator.find("bool IsReadbackLinearImagesEnabled() { return true; }") != std::string::npos);
    CHECK(emulator.find("s32 GetGpuId() { return -1; }") != std::string::npos);
    const std::string menu = ReadAll(source / "gpu" / "shim" / "dcp_overlay.cpp");
    for (const char* stale : {"Frame rate limit", "Frames queued", "Compile new shaders", "Performance",
                              "Upscaler", "Uncharted", "God of War", "fps_limit", "frames_queued"}) {
        CHECK(menu.find(stale) == std::string::npos);
    }
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    for (const char* hidden : {"async", "workers", "frame_limit", "fps_limit", "engine_fps", "frames_queued"}) {
        CHECK(launcher.find(hidden) == std::string::npos);
    }
    // The menu, the F11 save and the launcher only ever write these keys.
    for (const char* key : {"async_shaders", "fps_limit", "frames_queued", "upscaler", "engine_fps"}) {
        CHECK(ReadAll(source / "gpu" / "shim" / "dcp_settings.cpp").find(std::string("\"") + key + "\"") ==
              std::string::npos);
    }
}

// A package header with param.sfo as its only entry, `declared` bytes long by its header.
std::string MakePackage(const std::string& sfo, uint64_t declared = 0) {
    std::string pkg(0x1020, '\0');
    const auto be = [&](size_t at, uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            pkg[at + i] = char(value >> (8 * (bytes - 1 - i)));
        }
    };
    be(0x00, 0x7F434E54, 4);
    be(0x10, 1, 4);      // entries
    be(0x18, 0x1000, 4); // entry table
    be(0x1000, 0x1000, 4);
    be(0x1010, 0x1020, 4);
    be(0x1014, sfo.size(), 4);
    pkg += sfo;
    be(0x430, declared ? declared : pkg.size(), 8);
    return pkg;
}

// The installer refuses packages and folders that are not the supported game and never leaves a half-installed game
// behind: the previous installation stays as it was and no staging folder remains.
void TestInstaller(const fs::path& base) {
    Fixture source(base, "install-source");
    const GameRoots roots{base / "install-target" / "prepared", base / "install-target" / "image"};
    const GamePaths target = BuildPaths(roots, kUs);
    WriteAll(target.app0 / kPreparedMarker, "previous");
    WriteAll(target.image_dir / "boot.bin", "previous boot");
    const auto unchanged = [&] {
        return ReadAll(target.app0 / kPreparedMarker) == "previous" &&
               ReadAll(target.image_dir / "boot.bin") == "previous boot" &&
               !fs::exists(target.app0.parent_path() / "v1.03.install-temp") &&
               !fs::exists(target.image_dir.parent_path() / "v1.03.install-temp");
    };
    const auto install = [&](const fs::path& from, const GameRoots& to, const InstallHooks& hooks = {}) {
        return InstallGame(from, to, hooks, source.builds).status;
    };

    fs::create_directories(base / "install-empty");
    CHECK(install(base / "install-empty", roots) == InstallStatus::kNotGameFolder);
    // The synthetic eboot.bin is copied, then the boot image cannot be built from it.
    CHECK(install(source.paths.app0, roots) == InstallStatus::kPrepareFailed);
    CHECK(unchanged());
    InstallHooks cancel;
    cancel.progress = [](uint64_t, uint64_t) { return false; };
    CHECK(install(source.paths.app0, roots, cancel) == InstallStatus::kCancelled);
    CHECK(unchanged());

    const fs::path inside = target.app0 / "copy";
    WriteAll(inside / "eboot.bin", kEboot);
    WriteAll(inside / "sce_sys" / "param.sfo", ReadAll(source.paths.app0 / "sce_sys" / "param.sfo"));
    CHECK(install(inside, roots) == InstallStatus::kOverlapping);
    fs::remove_all(inside);

    const GameRoots foreign{base / "install-foreign" / "prepared", base / "install-foreign" / "image"};
    WriteAll(BuildPaths(foreign, kUs).app0 / "notes.txt", "mine");
    CHECK(install(source.paths.app0, foreign) == InstallStatus::kForeignFolder);
    CHECK(ReadAll(BuildPaths(foreign, kUs).app0 / "notes.txt") == "mine");

    const auto sfo_with = [](const char* title_id, const char* app_ver) {
        return MakeSfo({{"TITLE_ID", title_id},
                        {"CONTENT_ID", std::string(kUs.content_id)},
                        {"APP_VER", app_ver},
                        {"VERSION", std::string(FinishedLayer(kUs).sfo_version)}});
    };
    WriteAll(source.paths.app0 / "sce_sys" / "param.sfo", sfo_with("CUSA00001", "01.00"));
    CHECK(install(source.paths.app0, roots) == InstallStatus::kWrongGame);
    WriteAll(source.paths.app0 / "sce_sys" / "param.sfo", sfo_with("CUSA10454", "01.04"));
    CHECK(install(source.paths.app0, roots) == InstallStatus::kWrongVersion);
    source.Rebuild();
    fs::remove(source.paths.app0 / "DRACRO.CPK");
    CHECK(install(source.paths.app0, roots) == InstallStatus::kIncomplete);
    CHECK(unchanged());
    CHECK(std::string(DescribeInstall(InstallStatus::kNotGameFolder).text).find("eboot.bin") != std::string::npos);

    const fs::path pkg = base / "install-packages";
    const std::string sfo = ReadAll(source.paths.app0 / "sce_sys" / "param.sfo");
    WriteAll(pkg / "notes.pkg", "not a package");
    CHECK(install(pkg / "notes.pkg", roots) == InstallStatus::kNotPackage);
    CHECK(install(pkg / "missing.pkg", roots) == InstallStatus::kNotPackage);
    WriteAll(pkg / "short.pkg", MakePackage(sfo, 6ull << 30));
    CHECK(install(pkg / "short.pkg", roots) == InstallStatus::kIncomplete);
    WriteAll(pkg / "other.pkg", MakePackage(sfo_with("CUSA00001", "01.00")));
    CHECK(install(pkg / "other.pkg", roots) == InstallStatus::kWrongGame);
    WriteAll(pkg / "patch.pkg", MakePackage(sfo_with("CUSA10454", "01.03")));
    CHECK(install(pkg / "patch.pkg", roots) == InstallStatus::kWrongVersion);
    // The right game without the fake-package keys, as a package encrypted for a console looks here.
    WriteAll(pkg / "console.pkg", MakePackage(sfo));
    CHECK(install(pkg / "console.pkg", roots) == InstallStatus::kUnsupportedPackage);
    WriteAll(target.app0 / "game.pkg", MakePackage(sfo));
    CHECK(install(target.app0 / "game.pkg", roots) == InstallStatus::kOverlapping);
    fs::remove(target.app0 / "game.pkg");
    CHECK(unchanged());

    // The real package, when the development tree's game folder has one: cancelled during unpacking, nothing changes.
    fs::path real;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(DCP_SOURCE_DIR) / "game", ec)) {
        if (entry.path().extension() == ".pkg") {
            real = entry.path();
            break;
        }
    }
    if (!real.empty()) {
        InstallHooks cancel_unpacking;
        bool unpacking = false;
        cancel_unpacking.progress = [&](uint64_t done, uint64_t) { return !(unpacking = done > 0); };
        CHECK(InstallGame(real, roots, cancel_unpacking).status == InstallStatus::kCancelled);
        CHECK(unpacking);
        CHECK(unchanged());
    }
}

// The save list's pictures: a PNG decodes to RGBA rows (the icon's accent colour, its transparent corner), anything
// else is refused.
void TestPngDecoder() {
    const fs::path icon = fs::path(DCP_SOURCE_DIR) / "packaging" / "icons" / "app-icon-32.png";
    DcpPng::Image image;
    CHECK(DcpPng::LoadRgba(icon.string().c_str(), image));
    CHECK(image.width == 32 && image.height == 32 && image.rgba.size() == 32u * 32u * 4u);
    bool accent = false;
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        accent |= image.rgba[i] == 0xE0 && image.rgba[i + 1] == 0xBB && image.rgba[i + 2] == 0x32 &&
                  image.rgba[i + 3] == 0xFF;
    }
    CHECK(accent);
    CHECK(!image.rgba.empty() && image.rgba[3] == 0);
    const unsigned char truncated[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13};
    CHECK(!DcpPng::DecodeRgba(truncated, sizeof(truncated), image) && image.rgba.empty());
    CHECK(!DcpPng::LoadRgba((icon.string() + ".missing").c_str(), image));
}

// The video core reads dcp.ini as the launcher does, falls back to the defaults on values it does not accept and
// writes only the display mode (F11).
void TestVideoCoreSettings(const fs::path& base) {
    const fs::path file = base / "video-core" / "dcp.ini";
    fs::remove_all(file.parent_path());
    {
        DcpSettings::Values v;
        CHECK(!DcpSettings::Read(file.string(), v)); // no file: the defaults stay
        CHECK(v.vsync && !v.fsr1 && v.rcas && v.rcas_strength == 75 && !v.smaa && v.anisotropy == 0);
        CHECK(v.display_mode == DcpGraphics::Windowed && v.graphics_card.empty() && v.monitor == 1);
        CHECK(!v.show_fps && !v.show_fps_title && !v.show_resolution_title && v.requested_width == 0 &&
              v.requested_height == 0);
    }
    WriteAll(file,
             "# Dragon's Crown Pro user settings\n"
             "vsync = 0\n"
             "fsr1=1\n"
             "rcas = off\n"
             "rcas_strength = 40\n"
             "smaa = yes\n"
             "anisotropy = 16\n"
             "display_mode = borderless\n"
             "graphics_card = Intel(R) UHD Graphics 730 # the integrated one\n"
             "monitor = 2\n"
             "window_width = 2560\n"
             "window_height = 1440\n"
             "show_fps = true\n"
             "show_fps_title = 1\n"
             "show_resolution_title = on\n"
             "overlay_scale = 150\n"
             "overlay_corner = 3\n"
             "upscaler = fsr3\n"
             "async_shaders = 1\n"
             "fps_limit = 30\n"
             "frames_queued = 0\n");
    {
        DcpSettings::Values v;
        CHECK(DcpSettings::Read(file.string(), v));
        CHECK(!v.vsync && v.fsr1 && !v.rcas && v.rcas_strength == 40 && v.smaa && v.anisotropy == 16);
        CHECK(v.display_mode == DcpGraphics::Borderless);
        CHECK(v.graphics_card == "Intel(R) UHD Graphics 730"); // a trailing " # comment" is not part of the value
        CHECK(v.monitor == 2 && v.requested_width == 2560 && v.requested_height == 1440);
        CHECK(v.show_fps && v.show_fps_title && v.show_resolution_title);
        CHECK(v.overlay_scale == 150 && v.overlay_corner == 3);
    }

    // Values the game does not accept fall back to the defaults; the last of duplicate keys wins (as in the launcher).
    WriteAll(file,
             "vsync = maybe\n"
             "rcas = 2\n"
             "rcas_strength = 150\n"
             "anisotropy = 3\n"
             "display_mode = fullscreen\n"
             "graphics_card = automatic\n"
             "monitor = 0\n"
             "window_width = -5\n"
             "overlay_scale = 9000\n"
             "overlay_corner = 7\n"
             "smaa = 1\n"
             "smaa = 0\n"
             "not a setting line\n"
             "= 1\n");
    {
        DcpSettings::Values v;
        CHECK(DcpSettings::Read(file.string(), v));
        CHECK(v.vsync && v.rcas && v.rcas_strength == 75 && v.anisotropy == 0);
        CHECK(v.display_mode == DcpGraphics::Windowed && v.graphics_card.empty() && v.monitor == 1);
        CHECK(v.requested_width == 0 && v.overlay_scale == 300 && v.overlay_corner == 1);
        CHECK(!v.smaa);
    }

    // The launcher and the video core read the same file the same way.
    {
        LauncherSettings settings(file);
        settings.Set(kDisplayModeKey, "borderless");
        settings.SetBool(kVSyncKey, false);
        settings.Set(kAnisotropyKey, "8");
        settings.Set(kGraphicsCardKey, "NVIDIA GeForce RTX 4070");
        CHECK(settings.Save());
        DcpSettings::Values v;
        CHECK(DcpSettings::Read(file.string(), v));
        CHECK(v.display_mode == DcpGraphics::Borderless && !v.vsync && v.anisotropy == 8);
        CHECK(v.graphics_card == "NVIDIA GeForce RTX 4070");
    }

    // The F11 save rewrites the display mode in place and keeps every other line.
    WriteAll(file,
             "# header kept\n"
             "show_launcher = 0\n"
             "display_mode = windowed\n"
             "render_resolution = 4K\n"
             "launcher_width = 1280\n");
    DcpSettings::Load(file.string());
    auto& live = DcpSettings::Get();
    live.display_mode = DcpGraphics::Borderless;
    CHECK(DcpSettings::SaveDisplayMode());
    {
        const std::string text = ReadAll(file);
        CHECK(text.find("display_mode = borderless\n") != std::string::npos);
        CHECK(text.find("display_mode = windowed") == std::string::npos);
        CHECK(text.find("# header kept\n") == 0);
        CHECK(text.find("render_resolution = 4K") != std::string::npos);
        CHECK(text.find("vsync") == std::string::npos); // F11 writes the display mode alone
        CHECK(text.find("launcher_width = 1280") != std::string::npos);
        LauncherSettings reread(file);
        CHECK(reread.Get(kDisplayModeKey) == "borderless" && reread.GetBool(kShowLauncherKey, true) == false);
    }
}

// The window title the game composes: the game's title, then the render resolution and the frame rate when shown.
// A text entry takes the place of the frame rate and keeps the resolution.
void TestWindowTitle() {
    const auto compose = [](bool resolution, bool fps) {
        return DcpTitle::Compose({.base = "Dragon's Crown Pro", .show_resolution = resolution, .width = 2560,
                                  .height = 1440, .show_fps = fps, .fps = 60.0});
    };
    CHECK(compose(false, false) == "Dragon's Crown Pro");
    CHECK(compose(true, false) == "Dragon's Crown Pro | 2560x1440");
    CHECK(compose(false, true) == "Dragon's Crown Pro | FPS: 60.00");
    CHECK(compose(true, true) == "Dragon's Crown Pro | 2560x1440 | FPS: 60.00");

    CHECK(DcpTitle::Compose({.base = "G", .show_resolution = true, .show_fps = true}) == "G"); // nothing known yet
    CHECK(DcpTitle::Compose({.base = "G", .show_fps = true, .fps = -1.0}) == "G");
    CHECK(DcpTitle::Compose({.base = "G", .show_fps = true, .fps = 59.999}) == "G | FPS: 60.00");
    CHECK(DcpTitle::Compose({.base = "G", .show_resolution = true, .width = 3840, .height = 2160, .show_fps = true,
                             .fps = 60.0}) == "G | 3840x2160 | FPS: 60.00");
    CHECK(DcpTitle::Compose({.base = "G", .show_resolution = true, .width = 1280, .height = 720, .show_fps = true,
                             .fps = 30.0, .entry = " \u2014 Name: ab_"}) == "G | 1280x720 \u2014 Name: ab_");
}

// One default per setting: what the launcher shows for a missing key is what the game uses.
void TestDefaultsAgree() {
    const DcpSettings::Values game;
    const auto launcher_default = [](std::string_view key) {
        for (const ManagedSetting& setting : ManagedSettings()) {
            if (setting.key == key) {
                return std::string(setting.default_value);
            }
        }
        return std::string("<missing>");
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    CHECK(launcher_default(kDisplayModeKey) == std::string(DcpGraphics::DisplayModeKeys[game.display_mode]));
    CHECK(launcher_default(kVSyncKey) == flag(game.vsync));
    CHECK(launcher_default(kUpscaleKey) == flag(game.fsr1));
    CHECK(launcher_default(kSharpenKey) == flag(game.rcas));
    CHECK(launcher_default(kSharpenStrengthKey) == std::to_string(game.rcas_strength.load()));
    CHECK(launcher_default(kAntiAliasingKey) == flag(game.smaa));
    CHECK(launcher_default(kAnisotropyKey) == std::to_string(game.anisotropy.load()));
    CHECK(launcher_default(kGraphicsCardKey).empty() && game.graphics_card.empty());
    CHECK(launcher_default(kMonitorKey) == std::to_string(game.monitor.load()));
    CHECK(launcher_default(kShowFpsTitleKey) == flag(game.show_fps_title));
    CHECK(launcher_default(kShowResolutionTitleKey) == flag(game.show_resolution_title));
    CHECK(launcher_default(kShowFrameCounterKey) == flag(game.show_fps));
    // 0 is "the loader's size", 1920 x 1080 (src/loader.c), the launcher's default.
    CHECK(game.requested_width == 0 && launcher_default(kWindowWidthKey) == "1920");
    CHECK(game.requested_height == 0 && launcher_default(kWindowHeightKey) == "1080");
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c").find(",attributes,1920,1080,features};") != std::string::npos);

    // The launcher shows the default for values the game would not accept.
    CHECK(!IsValidSetting(kDisplayModeKey, "fullscreen") && IsValidSetting(kDisplayModeKey, "borderless"));
    CHECK(!IsValidSetting(kAnisotropyKey, "3") && IsValidSetting(kAnisotropyKey, "16"));
    CHECK(!IsValidSetting(kRenderResolutionKey, "8K") && IsValidSetting(kRenderResolutionKey, "4K"));
}

bool WrittenValue(const std::string& text, const std::string& key, const std::string& value);

void TestDetailedLogging(const fs::path& base) {
    const fs::path file = base / "logging" / "dcp.ini";
    fs::remove_all(file.parent_path());
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kDetailedLoggingKey && setting.default_value == "1";
    }
    CHECK(managed);
    CHECK(LauncherSettings(file).GetBool(kDetailedLoggingKey, true));
    CHECK(dcp_detailed_logging(file.string().c_str()) == 1);
    CHECK(dcp_detailed_logging(nullptr) == 1);

    WriteAll(file, "detailed_logging = 0\n");
    CHECK(!LauncherSettings(file).GetBool(kDetailedLoggingKey, true));
    CHECK(dcp_detailed_logging(file.string().c_str()) == 0);
    WriteAll(file, "detailed_logging = 1\n");
    CHECK(dcp_detailed_logging(file.string().c_str()) == 1);
    WriteAll(file, "detailed_logging = true\n");
    CHECK(dcp_detailed_logging(file.string().c_str()) == 1);
    WriteAll(file, "detailed_logging = garbage\n");
    CHECK(dcp_detailed_logging(file.string().c_str()) == 1);
    WriteAll(file, "vsync = 0\n");
    CHECK(dcp_detailed_logging(file.string().c_str()) == 1);

    {
        LauncherSettings settings(file);
        settings.SetBool(kDetailedLoggingKey, false);
        CHECK(settings.Save());
        CHECK(WrittenValue(ReadAll(file), kDetailedLoggingKey, "0"));
        CHECK(dcp_detailed_logging(file.string().c_str()) == 0);
    }
    {
        LauncherSettings settings(file);
        CHECK(!settings.GetBool(kDetailedLoggingKey, true));
        settings.ResetToDefaults();
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), kDetailedLoggingKey, "1"));
    CHECK(dcp_detailed_logging(file.string().c_str()) == 1);

    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("ui.Heading(\"Diagnostics\")") != std::string::npos);
    CHECK(launcher.find("ui.Toggle(\"Detailed logging\"") != std::string::npos);
    CHECK(launcher.find("Keeps logs for normal sessions. Crash diagnostics are always preserved.") != std::string::npos);
}

void TestLogsDirectory(const fs::path& base) {
    const fs::path root = base / "logs-directory";
    fs::remove_all(root);
    fs::create_directories(root / "nested");
    char out[4096];

    const fs::path default_dir = root / "logs";
    CHECK(dcp_logs_directory(Utf8(default_dir).c_str(), nullptr, out, sizeof(out)));
    CHECK(PathFromUtf8(out) == default_dir);
    CHECK(!fs::exists(default_dir));

    const fs::path log = root / "nested" / "run.log";
    CHECK(dcp_logs_directory(Utf8(default_dir).c_str(), Utf8(log).c_str(), out, sizeof(out)));
    CHECK(PathFromUtf8(out) == log.parent_path());

    const fs::path created_dir = root / "created";
    const std::string created = CreateLog(created_dir, nullptr, "run\n");
    CHECK(!created.empty());
    CHECK(dcp_logs_directory(Utf8(default_dir).c_str(), created.c_str(), out, sizeof(out)));
    CHECK(PathFromUtf8(out) == created_dir);

    CHECK(dcp_logs_directory(Utf8(default_dir).c_str(), "", out, sizeof(out)));
    CHECK(PathFromUtf8(out) == default_dir);
    CHECK(!dcp_logs_directory(nullptr, nullptr, out, sizeof(out)));
    CHECK(!dcp_logs_directory(Utf8(default_dir).c_str(), Utf8(log).c_str(), nullptr, sizeof(out)));
    CHECK(!dcp_logs_directory(Utf8(default_dir).c_str(), Utf8(log).c_str(), out, 0));

    const fs::path unicode =
        root / "logs with space" / PathFromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    CHECK(dcp_logs_directory(Utf8(unicode).c_str(), nullptr, out, sizeof(out)));
    CHECK(PathFromUtf8(out) == unicode);
    CHECK(fs::create_directories(unicode));
    CHECK(fs::is_directory(unicode));

    const fs::path source(DCP_SOURCE_DIR);
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    CHECK(launcher.find("ui.ButtonRow(\"Logs folder\"") != std::string::npos);
    CHECK(launcher.find("\"Open the folder containing session and crash logs\"") != std::string::npos);
    CHECK(launcher.find("\"Open logs folder\"") != std::string::npos);
    CHECK(launcher.find("OpenLogsFolder") != std::string::npos);
    CHECK(launcher.find("std::filesystem::create_directories(folder, ec)") != std::string::npos);
    CHECK(launcher.find("trg::OpenInFileManager(Utf8(folder))") != std::string::npos);

    const std::string loader = ReadAll(source / "src" / "loader.c");
    CHECK(loader.find("dcp_logs_directory(paths.logs_dir,log_path") != std::string::npos);
    CHECK(loader.find("DcpLauncherConfig launcher={app0,paths.image_dir,user_dir,settings_file,log_folder") !=
          std::string::npos);

    const std::string paths = ReadAll(source / "launcher" / "paths.cpp");
    CHECK(paths.find("DCP_PORTABLE_DIR") != std::string::npos);
    CHECK(paths.find("XDG_DATA_HOME") != std::string::npos);
    CHECK(paths.find("/tmp/.mount") == std::string::npos);
}

void TestLogRetention(const fs::path& base) {
    CHECK(runtime_log_should_remove(0, 1, 0));
    CHECK(!runtime_log_should_remove(1, 1, 0));
    CHECK(!runtime_log_should_remove(0, 0, 0));
    CHECK(!runtime_log_should_remove(0, 1, 1));

    const fs::path dir = base / "logging";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto session = [&](const char* name) {
        const fs::path file = dir / name;
        WriteAll(file, name);
        runtime_log_set_session_path(file.string().c_str());
        return file;
    };

    // Off: the session log is active, then a clean shutdown deletes it.
    const fs::path off_clean = session("dragons_crown_pro_001.log");
    runtime_log_set_detailed(0);
    CHECK(fs::exists(off_clean));
    runtime_log_mark_clean_exit();
    runtime_log_finalize();
    CHECK(!fs::exists(off_clean));

    // Off: a crash keeps the log, even with a clean mark present.
    const fs::path off_crash = session("dragons_crown_pro_002.log");
    runtime_log_set_detailed(0);
    runtime_log_mark_clean_exit();
    runtime_log_mark_crash();
    runtime_log_finalize();
    CHECK(fs::exists(off_crash));

    // Off: a path that never marked a clean exit is abnormal and keeps the log.
    const fs::path off_abnormal = session("dragons_crown_pro_003.log");
    runtime_log_set_detailed(0);
    runtime_log_finalize();
    CHECK(fs::exists(off_abnormal));

    // On: the session log is active and kept after a clean shutdown.
    const fs::path on_clean = session("dragons_crown_pro_004.log");
    runtime_log_set_detailed(1);
    CHECK(fs::exists(on_clean));
    runtime_log_mark_clean_exit();
    runtime_log_finalize();
    CHECK(fs::exists(on_clean));

    const fs::path on_crash = session("dragons_crown_pro_005.log");
    runtime_log_set_detailed(1);
    runtime_log_mark_clean_exit();
    runtime_log_mark_crash();
    runtime_log_finalize();
    CHECK(fs::exists(on_crash));

    // History: a new numbered log never overwrites an older one.
    const fs::path history = dir / "history";
    CHECK(fs::path(CreateLog(history, nullptr, "one")).filename() == "dragons_crown_pro_001.log");
    CHECK(fs::path(CreateLog(history, nullptr, "two")).filename() == "dragons_crown_pro_002.log");
    CHECK(fs::exists(history / "dragons_crown_pro_001.log"));

    const fs::path source(DCP_SOURCE_DIR);
    const std::string loader = ReadAll(source / "src" / "loader.c");
    CHECK(loader.find("runtime_log_install_cleanup()") != std::string::npos);
    CHECK(loader.find("dcp_detailed_logging(settings_file)") != std::string::npos);
    CHECK(loader.find("runtime_log_mark_clean_exit()") != std::string::npos);
    CHECK(loader.find("runtime_log_mark_crash()") != std::string::npos);
    CHECK(ReadAll(source / "gpu" / "shim" / "dcpgpu.cpp").find("runtime_log_finalize()") != std::string::npos);
}

void TestJapaneseNpcVoiceSetting(const fs::path& base) {
    const fs::path file = base / "npc_voices" / "dcp.ini";
    fs::remove_all(file.parent_path());
    const auto launcher_default = [](std::string_view key) {
        for (const ManagedSetting& setting : ManagedSettings()) {
            if (setting.key == key) {
                return std::string(setting.default_value);
            }
        }
        return std::string("<missing>");
    };
    CHECK(launcher_default(kJapaneseNpcVoicesKey) == "0");
    CHECK(dcp_japanese_npc_voices(nullptr) == 0);
    CHECK(dcp_japanese_npc_voices(file.string().c_str()) == 0);

    {
        LauncherSettings settings(file);
        settings.SetBool(kJapaneseNpcVoicesKey, true);
        CHECK(dcp_japanese_npc_voices(file.string().c_str()) == 0); // pending until Save
        CHECK(settings.Save());
    }
    CHECK(dcp_japanese_npc_voices(file.string().c_str()) == 1);
    CHECK(WrittenValue(ReadAll(file), kJapaneseNpcVoicesKey, "1"));
    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(kJapaneseNpcVoicesKey, false));
        settings.SetBool(kJapaneseNpcVoicesKey, false);
        CHECK(settings.Save());
    }
    CHECK(dcp_japanese_npc_voices(file.string().c_str()) == 0);
    CHECK(WrittenValue(ReadAll(file), kJapaneseNpcVoicesKey, "0"));
    WriteAll(file, "dcp_japanese_npc_voices = on\n");
    CHECK(dcp_japanese_npc_voices(file.string().c_str()) == 1);
    WriteAll(file, "dcp_japanese_npc_voices = garbage\n");
    CHECK(dcp_japanese_npc_voices(file.string().c_str()) == 0);

    // The Japanese builds already speak Japanese; every other build has the toggle, by table or by signature.
    CHECK(SupportsJapaneseNpcVoices(&kUs101));
    CHECK(SupportsJapaneseNpcVoices(&kCk100));
    CHECK(SupportsJapaneseNpcVoices(&kCk));
    for (const GameBuild* japan : {&kJp100, &kJp102, &kJp103}) {
        CHECK(!SupportsJapaneseNpcVoices(japan) && NativeJapaneseNpcVoices(japan));
    }
    CHECK(!NativeJapaneseNpcVoices(&kUs) && !NativeJapaneseNpcVoices(&kCk100) && !NativeJapaneseNpcVoices(nullptr));
    CHECK(!SupportsJapaneseNpcVoices(nullptr));
}

// Online play: managed keys with their defaults, the reader the game uses, validation, kept by Reset, and only the
// builds whose network imports are verified (docs/ONLINE.md) offer it. The lobby and UPnP are not settings.
void TestOnlineSettings(const fs::path& base) {
    const fs::path file = base / "online" / "dcp.ini";
    fs::remove_all(file.parent_path());
    const auto launcher_default = [](std::string_view key) {
        for (const ManagedSetting& setting : ManagedSettings()) {
            if (setting.key == key) {
                return std::string(setting.default_value);
            }
        }
        return std::string("<missing>");
    };
    CHECK(launcher_default(kOnlineEnabledKey) == "0" && launcher_default(kOnlineNameKey) == "Player");
    CHECK(launcher_default(kOnlineUdpPortKey) == "0" && launcher_default(kOnlineStunKey) == net::kDefaultStunServers);
    CHECK(launcher_default("online_lobby") == "<missing>" && launcher_default("online_upnp") == "<missing>");
    CHECK(launcher_default(kLobbyVisibleKey) == "0" && !IsOnlineSetting(kLobbyVisibleKey) && !net::OnlineConfig{}.lobby_visible);
    CHECK(!IsOnlineSetting("online_lobby") && !IsOnlineSetting("online_upnp"));
    CHECK(std::string(net::kDefaultLobbyUrl) == "wss://dcp-lobby.saulobenigno.workers.dev");
    const net::OnlineConfig missing = ReadOnlineConfig(file);
    CHECK(!missing.enabled && missing.upnp && missing.udp_port == 0 && missing.name == "Player");
    CHECK(missing.lobby == net::kDefaultLobbyUrl);
    CHECK(!ReadOnlineConfig({}).enabled && ReadOnlineConfig({}).lobby == net::kDefaultLobbyUrl);

    // Values a development dcp.ini may still carry change nothing: only DCP_LOBBY and DCP_UPNP do.
    WriteAll(file, "online_enabled = 1\nonline_lobby = wss://lobby.example.org/\nonline_name = Saulo\nonline_upnp = 0\n"
                   "online_udp_port = 40001\n");
    const net::OnlineConfig saved = ReadOnlineConfig(file);
    CHECK(saved.enabled && saved.name == "Saulo" && saved.udp_port == 40001);
    CHECK(saved.lobby == net::kDefaultLobbyUrl && saved.upnp);
    WriteAll(file, "online_udp_port = 70000\nonline_name = \n");
    const net::OnlineConfig invalid = ReadOnlineConfig(file);
    CHECK(invalid.udp_port == 0 && invalid.name == "Player");
    CHECK(IsValidSetting(kOnlineUdpPortKey, "3658") && !IsValidSetting(kOnlineUdpPortKey, "-1"));
    CHECK(IsValidSetting(kOnlineNameKey, std::string(64, 'a')) && !IsValidSetting(kOnlineNameKey, std::string(65, 'a')));

    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    };
    set_env("DCP_LOBBY", "ws://127.0.0.1:8790/");
    set_env("DCP_UPNP", "0");
    net::OnlineConfig local = ReadOnlineConfig(file);
    net::apply_environment(local);
    CHECK(local.lobby == "ws://127.0.0.1:8790/" && !local.upnp);
    set_env("DCP_LOBBY", nullptr);
    set_env("DCP_UPNP", nullptr);
    net::OnlineConfig production = ReadOnlineConfig(file);
    net::apply_environment(production);
    CHECK(production.lobby == net::kDefaultLobbyUrl && production.upnp);

    // The Online name is only a display name: renaming keeps the identity, which the lobby keys the friend code and
    // the friendships by, and dcp.ini never holds the identity.
    const net::Identity identity = net::load_identity(file.parent_path());
    {
        LauncherSettings settings(file);
        settings.Set(kOnlineNameKey, "Renamed");
        CHECK(settings.Save());
    }
    CHECK(ReadOnlineConfig(file).name == "Renamed" && LauncherSettings(file).Get(kOnlineNameKey) == "Renamed");
    const net::Identity renamed = net::load_identity(file.parent_path());
    CHECK(identity.ok() && renamed.account == identity.account && renamed.secret == identity.secret);
    CHECK(ReadAll(file).find(identity.secret) == std::string::npos &&
          ReadAll(file).find(std::to_string(identity.account)) == std::string::npos);

    WriteAll(file, "online_enabled = 1\nonline_name = Saulo\nvsync = 0\n");
    {
        LauncherSettings settings(file);
        settings.ResetToDefaults();
        CHECK(settings.Save());
    }
    CHECK(ReadOnlineConfig(file).enabled && ReadOnlineConfig(file).name == "Saulo");
    CHECK(LauncherSettings(file).GetBool(kVSyncKey, false));

    // Online play is offered for every supported build: no per-build flag or notice, one lobby game key for all.
    for (const char* source : {"launcher/game_profile.h", "launcher/game_profile.cpp", "launcher/dcp_launcher.h",
                               "launcher/game_files.cpp", "launcher/online_page.h", "launcher/online_page.cpp",
                               "launcher/online_view.h", "launcher/launcher.cpp", "src/loader.c"}) {
        const std::string text = ReadAll(fs::path(DCP_SOURCE_DIR) / source);
        for (const char* gate : {"build_->online", "build->online", "game_build.online", "game_build.skip_offers",
                                 "kUnavailable", "Supported()", "Online play is not available", "verified for the USA"})
            CHECK(text.find(gate) == std::string::npos);
    }
    const std::string page = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp");
    CHECK(page.find("client_.Update(on, config, net::kOnlineGame, now);") != std::string::npos);
    CHECK(page.find("game = std::string(net::kOnlineGame)") != std::string::npos);
    CHECK(SupportedBuilds().size() == 8);
}

// Each friend's line under the name: presence, then the platform and port version that friend's own client advertised,
// kept per account; "Incompatible" only for a room of another online protocol, never a protocol number.
void TestFriendStatus(const std::string& page) {
    const int current = net::kOnlineProtocolVersion, other = net::kOnlineProtocolVersion + 6;
    const std::string v = net::port_version();
    const auto parsed = net::Json::parse(
        "[{\"account\": \"11\", \"name\": \"Win\", \"online\": true, \"platform\": \"Windows\", \"version\": \"" + v +
        "\", \"online_protocol\": " + std::to_string(current) + "},"
        " {\"account\": \"12\", \"name\": \"Lin\", \"online\": true, \"platform\": \"Linux\", \"version\": \"" + v +
        "\", \"online_protocol\": " + std::to_string(current) + "},"
        " {\"account\": \"13\", \"name\": \"Deck\", \"online\": true, \"platform\": \"Steam Deck\", \"version\": \"" + v +
        "\", \"online_protocol\": " + std::to_string(current) + "},"
        " {\"account\": \"14\", \"name\": \"Host\", \"online\": true, \"room\": \"4097\", \"platform\": \"Linux\","
        " \"version\": \"" + v + "\", \"online_protocol\": " + std::to_string(current) + "},"
        " {\"account\": \"15\", \"name\": \"Patch\", \"online\": true, \"room\": \"4098\", \"platform\": \"Windows\","
        " \"version\": \"99.0.1\", \"online_protocol\": " + std::to_string(current) + "},"
        " {\"account\": \"16\", \"name\": \"Newer\", \"online\": true, \"room\": \"4099\", \"platform\": \"Steam Deck\","
        " \"version\": \"0.9.0\", \"online_protocol\": " + std::to_string(other) + "},"
        " {\"account\": \"17\", \"name\": \"Old\", \"online\": true},"
        " {\"account\": \"18\", \"name\": \"OldHost\", \"online\": true, \"room\": \"4100\"},"
        " {\"account\": \"19\", \"name\": \"Away\", \"online\": false, \"platform\": \"Windows\", \"version\": \"0.8.0\"}]");
    CHECK(parsed.has_value());
    const std::vector<OnlinePlayer> f = ReadOnlinePlayers(parsed ? *parsed : net::Json::Array());
    CHECK(f.size() == 9);
    if (f.size() != 9) return;
    const std::string dot = " \xC2\xB7 ";
    CHECK(FriendStatusOf(f[0]).text == "Online" + dot + "Windows" + dot + "v" + v && !FriendStatusOf(f[0]).incompatible);
    CHECK(FriendStatusOf(f[1]).text == "Online" + dot + "Linux" + dot + "v" + v);
    CHECK(FriendStatusOf(f[2]).text == "Online" + dot + "Steam Deck" + dot + "v" + v);
    CHECK(FriendStatusOf(f[3]).text == "Online, in a room" + dot + "Linux" + dot + "v" + v && !FriendStatusOf(f[3]).incompatible);
    CHECK(FriendStatusOf(f[4]).text == "Online, in a room" + dot + "Windows" + dot + "v99.0.1" && !FriendStatusOf(f[4]).incompatible);
    CHECK(FriendStatusOf(f[5]).text == "Online, in a room" + dot + "Steam Deck" + dot + "v0.9.0" && FriendStatusOf(f[5]).incompatible);
    CHECK(FriendStatusOf(f[6]).text == "Online" && !FriendStatusOf(f[6]).incompatible);
    CHECK(FriendStatusOf(f[7]).text == "Online, in a room" && FriendStatusOf(f[7]).incompatible);
    CHECK(FriendStatusOf(f[8]).text == "Offline" && !FriendStatusOf(f[8]).incompatible);
    for (const OnlinePlayer& player : f) {
        const std::string text = FriendStatusOf(player).text;
        for (const char* hidden : {"rotocol", "unknown", "missing", "PlayStation", "Incompatible", "("}) {
            CHECK(text.find(hidden) == std::string::npos);
        }
        CHECK(text.find(std::to_string(other)) == std::string::npos && text.find(player.account) == std::string::npos);
    }

    // Same Online name, different accounts; then a rename: the metadata follows the account, never the name.
    const auto twins = net::Json::parse(R"([{"account": "21", "name": "Player", "online": true, "platform": "Windows",
        "version": "0.8.0"}, {"account": "22", "name": "Player", "online": true, "platform": "Linux", "version": "0.8.1"}])");
    const auto renamed = net::Json::parse(R"([{"account": "21", "name": "Ana", "online": true, "platform": "Windows",
        "version": "0.8.0"}, {"account": "22", "name": "Ana2", "online": true, "platform": "Linux", "version": "0.8.1"}])");
    CHECK(twins.has_value() && renamed.has_value());
    for (const auto* rows : {&twins, &renamed}) {
        const std::vector<OnlinePlayer> p = ReadOnlinePlayers(rows->has_value() ? **rows : net::Json::Array());
        CHECK(p.size() == 2 && p[0].account == "21" && p[0].platform == "Windows" && p[0].version == "0.8.0");
        CHECK(p.size() == 2 && p[1].account == "22" && p[1].platform == "Linux" && p[1].version == "0.8.1");
    }

    // Request cards keep their two plain lines; the client asks the lobby nothing new for this.
    const size_t cards = page.find("void OnlinePage::DrawFriendRequests(");
    const size_t friends = page.find("void OnlinePage::DrawFriends(");
    CHECK(cards != std::string::npos && friends != std::string::npos);
    if (cards != std::string::npos && friends != std::string::npos) {
        const std::string card = page.substr(cards, friends - cards);
        CHECK(card.find("FriendStatusOf") == std::string::npos && card.find("platform") == std::string::npos);
        CHECK(card.find("sender.name + Tr(\" wants to be your friend\")") != std::string::npos);
    }
    CHECK(page.find("online_protocol") == std::string::npos);
    const std::string client = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_client.cpp");
    size_t sends = 0;
    for (size_t at = client.find("Send(\""); at != std::string::npos; at = client.find("Send(\"", at + 1)) ++sends;
    CHECK(sends == 10);
    for (const char* op : {"friends.list", "friends.requests", "friends.add", "friends.accept", "friends.decline",
                           "friends.remove", "lobby.publish", "lobby.list"}) {
        CHECK(client.find(std::string("Send(\"") + op + "\"") != std::string::npos);
    }
}

// The rows of the public Lobby: a handle, a name and the relation, whose action follows from it. The handle is never
// drawn, and a row missing either is dropped.
void TestLobbyRows(const std::string& page) {
    const auto parsed = net::Json::parse(
        R"([{"player": "a1", "name": "Stranger", "state": "none"}, {"player": "a2", "name": "Waiting", "state": "pending"},
            {"player": "a3", "name": "Pal", "state": "friend"}, {"player": "a4", "name": "Asking", "state": "incoming"},
            {"player": "a5", "name": "Later", "state": "something new"}, {"player": "", "name": "NoHandle", "state": "none"},
            {"player": "a7", "name": "", "state": "none"}, {"player": "a8"}])");
    CHECK(parsed.has_value());
    const std::vector<LobbyPlayer> rows = ReadLobbyPlayers(parsed ? *parsed : net::Json::Array());
    CHECK(rows.size() == 5);
    if (rows.size() != 5) return;
    using R = LobbyRelation;
    CHECK(rows[0].relation == R::kIncoming && rows[1].relation == R::kPending && rows[2].relation == R::kFriend);
    CHECK(rows[3].relation == R::kNone && rows[3].name == "Later" && rows[4].relation == R::kNone && rows[4].player == "a1" &&
          rows[4].name == "Stranger");
    // The name is the one the lobby shows; a client that said no version still gets a row.
    CHECK(rows[0].version.empty() && LobbyVersionLabel(rows[0]) == "Version unknown");
    const auto versioned = net::Json::parse(R"([{"player": "b1", "name": "Player-7A3C", "version": "0.9.2", "state": "none"}])");
    const std::vector<LobbyPlayer> known = ReadLobbyPlayers(versioned ? *versioned : net::Json::Array());
    CHECK(known.size() == 1 && known[0].name == "Player-7A3C" && LobbyVersionLabel(known[0]) == "v0.9.2");

    // Friends use the alias where the lobby sent one and the name otherwise; the game's own matching reads "name" directly.
    const auto aliased = net::Json::parse(R"([{"account": "1", "name": "Player", "display_name": "Player-42DF"},
                                              {"account": "2", "name": "Ana"}])");
    const std::vector<OnlinePlayer> friends = ReadOnlinePlayers(aliased ? *aliased : net::Json::Array());
    CHECK(friends.size() == 2 && friends[0].name == "Player-42DF" && friends[1].name == "Ana");
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "runtime_matching2.cpp").find("row.name = SafeName(f[\"name\"].as_string());") !=
          std::string::npos);

    // Requests first, then pending, friends and everyone else; by name without regard to case within each.
    const auto mixed = net::Json::parse(
        R"([{"player": "c1", "name": "zed", "state": "none"}, {"player": "c2", "name": "Bob", "state": "friend"},
            {"player": "c3", "name": "ana", "state": "none"}, {"player": "c4", "name": "Yan", "state": "pending"},
            {"player": "c5", "name": "Xia", "state": "incoming"}, {"player": "c6", "name": "Abe", "state": "incoming"},
            {"player": "c7", "name": "Bea", "state": "none"}, {"player": "c8", "name": "Al", "state": "friend"}])");
    std::string order;
    for (const LobbyPlayer& p : ReadLobbyPlayers(mixed ? *mixed : net::Json::Array())) order += p.name + " ";
    CHECK(order == "Abe Xia Yan Al Bob ana Bea zed ");

    // Five minutes by itself; the button and friend events do not wait for that, and only one read runs at a time.
    CHECK(LobbyRefresh::kAutoSeconds == 300.0);
    LobbyRefresh refresh;
    CHECK(refresh.Due(100.0));
    refresh.Start(100.0);
    CHECK(refresh.active() && !refresh.Due(100.0) && !refresh.Manual(120.0));
    refresh.Finish();
    CHECK(!refresh.Due(399.0) && refresh.Due(400.0));
    CHECK(!refresh.Manual(100.0 + LobbyRefresh::kManualGapSeconds - 0.1) && !refresh.Due(104.0));
    CHECK(refresh.Manual(100.0 + LobbyRefresh::kManualGapSeconds) && refresh.Due(105.0));
    refresh.Start(105.0);
    refresh.Soon(105.5);
    CHECK(!refresh.Due(105.6));
    refresh.Finish();
    CHECK(refresh.Due(105.6));
    refresh.Start(200.0);
    refresh.Fail(201.0);
    CHECK(!refresh.Due(230.0) && refresh.Due(231.0));
    refresh.Start(300.0);
    refresh.Reset();
    CHECK(refresh.Due(300.0) && !refresh.active());

    CHECK(LobbyUpdatedText(0) == "Updated just now" && LobbyUpdatedText(2) == "Updated 2 min ago");

    const LobbyAction send = LobbyActionOf(R::kNone), pending = LobbyActionOf(R::kPending), pal = LobbyActionOf(R::kFriend),
                      accept = LobbyActionOf(R::kIncoming);
    CHECK(std::string(send.text) == "Send friend request" && send.enabled && !send.primary);
    CHECK(std::string(pending.text) == "Pending" && !pending.enabled);
    CHECK(std::string(pal.text) == "Friend" && !pal.enabled);
    CHECK(std::string(accept.text) == "Accept" && accept.enabled && accept.primary && accept.decline);
    CHECK(!send.decline && !pending.decline && !pal.decline);

    const size_t draw = page.find("void OnlinePage::DrawLobbyPlayer(");
    const size_t end = page.find("void OnlinePage::DrawLobbyHeader(");
    CHECK(draw != std::string::npos && end > draw);
    if (draw == std::string::npos || end <= draw) return;
    const std::string row = page.substr(draw, end - draw);
    CHECK(row.find("ui.RowLeft(player.name.c_str());") != std::string::npos && row.find("ImGui::BeginDisabled();") != std::string::npos);
    CHECK(row.find("client_.RequestFromLobby(player.player);") != std::string::npos);
    CHECK(row.find("client_.DeclineFromLobby(player.player);") != std::string::npos);
    CHECK(row.find("Tr(\"Decline\")") != std::string::npos && row.find("LobbyVersionLabel(player)") != std::string::npos);
    CHECK(row.find("player.player.c_str()") == row.rfind("player.player.c_str()") && row.find("account") == std::string::npos);
    const std::string client = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_client.cpp");
    const size_t request = client.find("void OnlineClient::RequestFromLobby(");
    CHECK(request != std::string::npos && client.find("Send(\"friends.add\"", request) != std::string::npos);
    CHECK(client.find("Send(\"friends.decline\"", request) != std::string::npos);
    // A failed read leaves the rows as they were: only a good answer replaces them, and nothing empties them while listed.
    const size_t list = client.find("case Purpose::kLobbyList:\n        if (ok) {");
    const size_t failed = client.find("lobby_refresh_.Fail(now);", list);
    CHECK(list != std::string::npos && failed != std::string::npos);
    CHECK(client.find("state_.lobby = ReadLobbyPlayers", list) < failed && client.find("state_.lobby.clear()", failed) == std::string::npos);
    CHECK(client.find("if (!visible) ForgetLobby();") != std::string::npos);
    for (const char* never : {"\"room.", "room.create", "room.join", "chat"}) CHECK(client.find(never) == std::string::npos);
    CHECK(page.find("kLobbyVisibleKey, false, \"Off\", \"On\");") != std::string::npos);
    CHECK(page.find("client_.WatchLobby(") != std::string::npos);
}

// The Online page: with online play off only its switch; on, the tabs under it and the sections of the selected tab.
// Settings comes first and is what a blocked PLAY opens. The switch shows the effective state. No lobby address, UPnP
// switch or status card; friends by Online name.
void TestOnlinePage() {
    using S = OnlineSection;
    using T = OnlineTab;
    for (const T tab : kOnlineTabs) {
        CHECK(OnlinePageSections(false, tab, 0, false) == std::vector<S>{S::kOnlinePlay});
        CHECK(OnlinePageSections(false, tab, 2, true) == std::vector<S>{S::kOnlinePlay});
        const std::vector<S> on = OnlinePageSections(true, tab, 2, true);
        CHECK(on.size() >= 3 && on[0] == S::kOnlinePlay && on[1] == S::kTabs);
    }
    const std::vector<S> settings_tab{S::kOnlinePlay, S::kTabs, S::kOnlineName, S::kFriendCode, S::kConnectionTest,
                                      S::kPingDisplay, S::kEarlyStoryOnline};
    CHECK(OnlinePageSections(true, T::kSettings, 0, false) == settings_tab);
    CHECK(OnlinePageSections(true, T::kSettings, 3, true) == settings_tab);
    CHECK(OnlinePageSections(true, T::kFriends, 0, false) == (std::vector<S>{S::kOnlinePlay, S::kTabs, S::kFriends}));
    CHECK(OnlinePageSections(true, T::kFriends, 1, false) ==
          (std::vector<S>{S::kOnlinePlay, S::kTabs, S::kFriendRequests, S::kFriends}));
    CHECK(OnlinePageSections(true, T::kLobby, 1, false) == (std::vector<S>{S::kOnlinePlay, S::kTabs, S::kLobbyVisibility}));
    CHECK(OnlinePageSections(true, T::kLobby, 1, true) ==
          (std::vector<S>{S::kOnlinePlay, S::kTabs, S::kLobbyVisibility, S::kLobbyPlayers}));
    CHECK(std::size(kOnlineTabs) == 3 && std::string(OnlineTabLabel(kOnlineTabs[0])) == "Settings" &&
          std::string(OnlineTabLabel(kOnlineTabs[1])) == "Friends" && std::string(OnlineTabLabel(kOnlineTabs[2])) == "Lobby");
    const std::string page_header = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.h");
    CHECK(page_header.find("OnlineTab tab_ = OnlineTab::kSettings;") != std::string::npos);
    CHECK(page_header.find("tab_ = OnlineTab::kSettings;\n        pointed_ = true;") != std::string::npos);

    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    };
    set_env("DCP_ONLINE", nullptr);
    CHECK(!ResolveOnlinePlay(false).on && !ResolveOnlinePlay(false).locked);
    CHECK(ResolveOnlinePlay(true).on && !ResolveOnlinePlay(true).locked);
    set_env("DCP_ONLINE", "1");
    CHECK(ResolveOnlinePlay(false).on && ResolveOnlinePlay(false).locked);
    set_env("DCP_ONLINE", "0");
    CHECK(!ResolveOnlinePlay(true).on && ResolveOnlinePlay(true).locked);
    for (const char* value : {"1", "0", "on", "off"}) {
        set_env("DCP_ONLINE", value);
        net::OnlineConfig config;
        config.enabled = false;
        net::apply_environment(config);
        CHECK(ResolveOnlinePlay(false).on == config.enabled);
    }
    set_env("DCP_ONLINE", nullptr);

    const std::string page = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp");
    CHECK(!page.empty() && page.find("StatusCard") == std::string::npos);
    for (const char* hidden : {"Lobby server", "UPnP", "online_lobby", "online_upnp", "workers.dev", "Cloudflare",
                               "WebSocket", "\"Refresh\"", "\"Friend code\""}) {
        CHECK(page.find(hidden) == std::string::npos);
    }
    for (const char* shown : {"\"Online play\"", "\"Online name\"", "\"Your friend code\"", "\"Connection test\"",
                              "\"Run test\"", "\"Add a friend\"", "\"Send request\"", "\"XXXX-XXXX\"",
                              "\"New friend request\"", "Accept\"", "\"Decline\"", "\"Show me in Lobby\"",
                              "\"Players in Lobby\""}) {
        CHECK(page.find(shown) != std::string::npos);
    }
    CHECK(page.find("ui.InputText(\"Online name\", kNameHelp, kOnlineNameKey, net::kDefaultOnlineName)") != std::string::npos);
    CHECK(page.find("FriendRow(ui, f);") != std::string::npos);
    CHECK(page.find("ImGui::TextColored(ui.theme().warn_dot, \"%s\", Tr(\"Incompatible\"));") != std::string::npos);
    CHECK(page.find("ResolveOnlinePlay(") != std::string::npos);
    // The sidebar badge counts the requests the page shows as cards.
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("online_page_->PendingRequests()") != std::string::npos);
    CHECK(launcher.find("online_page_->Tick(settings_") != std::string::npos);
    const std::string badge = ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "patches" / "trg-launcher" / "0004-sidebar-badge.patch");
    CHECK(badge.find("std::function<int()> badge;") != std::string::npos);

    const auto rows = net::Json::parse(R"([{"account": "123456789012", "name": "Saulo", "online": true, "room": "77",
                                            "ip": "203.0.113.9", "port": 3658}, {"account": "5", "name": "", "online": false}])");
    CHECK(rows.has_value());
    const std::vector<OnlinePlayer> players = ReadOnlinePlayers(*rows);
    CHECK(players.size() == 2 && players[0].name == "Saulo" && players[0].online && players[0].in_room);
    CHECK(players[1].name == "Player" && !players[1].online && !players[1].in_room);
    CHECK(std::string(FriendPresence(players[0])) == "Online, in a room" && std::string(FriendPresence(players[1])) == "Offline");
    CHECK(FriendRequestText(players[0]) == "Saulo wants to be your friend");
    for (const OnlinePlayer& player : players) {
        for (const std::string& shown : {player.name + FriendPresence(player), FriendRequestText(player)}) {
            CHECK(shown.find(player.account) == std::string::npos && shown.find("203.0.113") == std::string::npos &&
                  shown.find("3658") == std::string::npos && shown.find("Client") == std::string::npos);
        }
    }

    TestFriendStatus(page);
    TestLobbyRows(page);

    // The release page shows no test-instance note, environment override, address or service name; a switch locked by
    // DCP_ONLINE reads plain On or Off.
    for (const char* hidden : {"localhost", "127.0.0.1", "8790", "ws://", "wss://", "\"Server\"", "\"Lobby\"",
                               "Test instance", "DCP_", "getenv", "config.lobby", "kDefaultLobbyUrl", "dcp-lobby"}) {
        CHECK(page.find(hidden) == std::string::npos);
    }
    CHECK(page.find("ui.Info(\"Online play\", online.on ? \"On\" : \"Off\");") != std::string::npos);
    CHECK(page.find("ui.Toggle(\"Online play\", kOnlineHelp, kOnlineEnabledKey, false);") != std::string::npos);
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.h").find("TestInstance") == std::string::npos);
    CHECK(launcher.find("Test instance") == std::string::npos);
    CHECK(std::string(net::kDefaultLobbyUrl) == "wss://dcp-lobby.saulobenigno.workers.dev");

    // Test instances use the production service with no lobby option or local lobby, and their own switch and Online
    // name unless told otherwise; only a DCP_LOBBY the caller set replaces the service.
    const std::string instance = ReadAll(fs::path(DCP_SOURCE_DIR) / "tools" / "online-instance.ps1");
    for (const char* gone : {"$Lobby", "-Lobby", "8790", "127.0.0.1", "localhost", "online-lobby", "workers.dev",
                             "$env:DCP_LOBBY =", "Env:DCP_LOBBY"}) {
        CHECK(instance.find(gone) == std::string::npos);
    }
    CHECK(instance.find("Online test instance $($Name.ToUpper())") != std::string::npos);
    CHECK(instance.find("Online service: $(if ($env:DCP_LOBBY) { \"DCP_LOBBY override $env:DCP_LOBBY\" } else { 'production' })") !=
          std::string::npos);
    CHECK(instance.find("tools\\online-instance.ps1 -Name a ") != std::string::npos &&
          instance.find("tools\\online-instance.ps1 -Name b ") != std::string::npos);
    CHECK(instance.find("if ($ForceOnline) { $env:DCP_ONLINE = '1' } else { Remove-Item Env:DCP_ONLINE ") != std::string::npos);
    CHECK(instance.find("$env:DCP_ONLINE = '1'\n") == std::string::npos);
    CHECK(instance.find("if ($OnlineName) { $env:DCP_ONLINE_NAME = $OnlineName } else { Remove-Item Env:DCP_ONLINE_NAME") !=
          std::string::npos);
}

// In-game notices use the frame counter's panel and drawing, in the top corner the frame counter leaves free.
void TestOverlayPanel() {
    namespace B = DcpOverlayBox;
    const float size = 18.0f, text_w = 120.0f, text_h = 18.0f;
    for (const B::Rect work : {B::Rect{0, 0, 1920, 1080}, B::Rect{0, 0, 1280, 720}, B::Rect{100, 50, 900, 650}}) {
        for (int corner = 0; corner < 4; ++corner) {
            // The frame counter's geometry as it was before notices shared it.
            const float pad_x = size * 0.6f, pad_y = size * 0.3f;
            const float w = text_w + pad_x * 2, h = text_h + pad_y * 2;
            const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
            const float x0 = right ? work.x1 - size - w : work.x0 + size;
            const float y0 = bottom ? work.y1 - size - h : work.y0 + size;
            const B::Box fps = B::Place(corner, text_w, text_h, size, work);
            CHECK(fps.rect.x0 == x0 && fps.rect.y0 == y0 && fps.rect.x1 == x0 + w && fps.rect.y1 == y0 + h);
            CHECK(fps.pad_x == pad_x && fps.pad_y == pad_y && fps.rounding == size * 0.5f);

            const int notice_corner = B::NoticeCorner(corner);
            CHECK(notice_corner != corner && notice_corner <= 1);
            const B::Box notice = B::Place(notice_corner, 420.0f, text_h, size, work);
            CHECK(!B::Overlap(fps.rect, notice.rect));
            CHECK(notice.pad_x == fps.pad_x && notice.pad_y == fps.pad_y && notice.rounding == fps.rounding);
        }
    }
    CHECK(B::NoticeCorner(1) == 0 && B::NoticeCorner(0) == 1);
    CHECK(B::kBorderThickness == 1.0f);
    CHECK(B::Overlap({0, 0, 10, 10}, {5, 5, 15, 15}) && !B::Overlap({0, 0, 10, 10}, {10, 0, 20, 10}));

    const std::string overlay = ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "dcp_overlay.cpp");
    CHECK(overlay.find("DrawOverlayPanel(FpsCounterBox(label.c_str()), label.c_str());") != std::string::npos);
    CHECK(overlay.find("DcpOverlayBox::NoticeCorner(DcpSettings::Get().overlay_corner)") != std::string::npos);
    CHECK(overlay.find("DrawOverlayPanel(PlaceOverlayPanel(notice_text.c_str(), corner), notice_text.c_str());") !=
          std::string::npos);
    CHECK(overlay.find("##notice") == std::string::npos);
    size_t panels = 0;
    for (size_t at = overlay.find("IM_COL32(7, 10, 22, 190)"); at != std::string::npos;
         at = overlay.find("IM_COL32(7, 10, 22, 190)", at + 1)) {
        ++panels;
    }
    CHECK(panels == 1);
    CHECK(overlay.find("return PlaceOverlayPanel(text, DcpSettings::Get().overlay_corner);") != std::string::npos);
    CHECK(overlay.find("if (DcpSettings::Get().show_fps) {\n        FpsCounter();") != std::string::npos);
}

// Insert and R3 + L2 open nothing: that settings menu stays removed. The Options menu (F1, L3 + R3; see
// tests/options_menu_tests.cpp) is a separate design. F10, F11, F2 and F3 stay.
void TestNoSettingsMenu() {
    const fs::path source(DCP_SOURCE_DIR);
    const std::string overlay =
        ReadAll(source / "gpu" / "shim" / "dcp_overlay.cpp") + ReadAll(source / "gpu" / "shim" / "dcp_overlay.h");
    const std::string window = ReadAll(source / "gpu" / "shim" / "window.cpp");
    const std::string settings = ReadAll(source / "gpu" / "shim" / "dcp_settings.cpp");
    for (const char* gone : {"SDLK_INSERT", "SDL_SCANCODE_INSERT", "R3+L2", "menu_open", "SetOpen(", "UpdateTextInput",
                             "Overlay: menu ready", "ImageTab", "OverlayTab", "menu_tab", "DcpSettings::Save()"}) {
        CHECK(overlay.find(gone) == std::string::npos);
        CHECK(window.find(gone) == std::string::npos);
    }
    // The window reads R3 and L2 for the game; the overlay no longer watches them.
    CHECK(overlay.find("SDL_GAMEPAD_BUTTON_RIGHT_STICK") == std::string::npos &&
          overlay.find("SDL_GAMEPAD_AXIS_LEFT_TRIGGER") == std::string::npos);
    CHECK(settings.find("menu_tab") == std::string::npos && settings.find("bool Save()") == std::string::npos);
    // Keys and buttons go to the game unless a menu or a game dialog has them.
    CHECK(overlay.find("        if (!MenuOpen()) {\n            return false;\n        }\n        io.AddKeyEvent(ImGuiMod_Ctrl") !=
          std::string::npos);
    CHECK(overlay.find("return cheat_open || options_open || text_entry_active || choice_active;") != std::string::npos);
    CHECK(overlay.find("if (event.key.scancode == DcpSettings::Get().menu_key) {") != std::string::npos);
    CHECK(window.find("if (window_shortcut && !event.key.repeat) {\n                ToggleBorderless();") != std::string::npos);
    CHECK(window.find("hud == DcpPing::HudKey::Fps") != std::string::npos &&
          window.find("hud == DcpPing::HudKey::Ping") != std::string::npos);
    CHECK(dcp_key_from_name("insert") == SDL_SCANCODE_INSERT); // still a key the game controls can use

    for (const char* doc : {"README.md", "docs/LAUNCHER.md", "docs/LINUX.md", "docs/GRAPHICS.md", "docs/CHEATS.md",
                            "packaging/README.txt", "packaging/README-linux.txt"}) {
        const std::string text = ReadAll(source / doc);
        CHECK(!text.empty());
        for (const char* stale : {"R3 + L2", "R3+L2", "Insert", "in-game settings menu", "menu_tab"}) {
            if (text.find(stale) != std::string::npos) {
                std::printf("stale settings menu reference in %s: %s\n", doc, stale);
                CHECK(text.find(stale) == std::string::npos);
            }
        }
    }
}

// F2 is the frame counter and F3 the Ping (Online Play only), each on its own; the Ping reads the P2P round trip a few
// times a second and sits next to the frame counter in its corner. The Online page's Ping display sets its start.
void TestPingDisplay(const fs::path& base) {
    using DcpPing::HudKey;
    for (const bool online : {false, true}) {
        CHECK(DcpPing::HudKeyFor(SDL_SCANCODE_F2, false, online) == HudKey::Fps);
        CHECK(DcpPing::HudKeyFor(SDL_SCANCODE_F2, true, online) == HudKey::None);
        CHECK(DcpPing::HudKeyFor(SDL_SCANCODE_F3, true, online) == HudKey::None);
        CHECK(DcpPing::HudKeyFor(SDL_SCANCODE_F4, false, online) == HudKey::None);
    }
    CHECK(DcpPing::HudKeyFor(SDL_SCANCODE_F3, false, false) == HudKey::None);
    CHECK(DcpPing::HudKeyFor(SDL_SCANCODE_F3, false, true) == HudKey::Ping);

    CHECK(DcpPing::Text(-1) == "Ping: --" && DcpPing::Text(0) == "Ping: 0 ms" && DcpPing::Text(28) == "Ping: 28 ms");
    DcpPing::Readout readout;
    int samples = 0, rtt = -1;
    const auto sample = [&] {
        ++samples;
        return rtt;
    };
    CHECK(readout.Shown(1000, sample) == -1 && samples == 1); // no peer yet
    rtt = 28;
    CHECK(readout.Shown(1100, sample) == -1 && samples == 1); // held until the next read
    CHECK(readout.Shown(1300, sample) == 28 && samples == 2);
    rtt = 41;
    CHECK(readout.Shown(1599, sample) == 28 && readout.Shown(1600, sample) == 41 && samples == 3);
    rtt = -1; // the peer left
    CHECK(readout.Shown(1900, sample) == -1 && samples == 4);
    rtt = 30;
    readout.Reset(); // shown again: read at once
    CHECK(readout.Shown(1901, sample) == 30 && samples == 5);
    CHECK(DcpPing::Readout::kRefreshMs >= 250 && DcpPing::Readout::kRefreshMs <= 500);

    namespace B = DcpOverlayBox;
    const float size = 18.0f;
    for (const B::Rect work : {B::Rect{0, 0, 1920, 1080}, B::Rect{0, 0, 1280, 720}}) {
        for (int corner = 0; corner < 4; ++corner) {
            const B::Box fps = B::Place(corner, 150.0f, size, size, work);
            const B::Box alone = B::Place(corner, 90.0f, size, size, work);
            const B::Box ping = B::StackAfter(fps, corner, 90.0f, size, size, work);
            CHECK(!B::Overlap(fps.rect, ping.rect));
            CHECK(ping.rect.x0 == alone.rect.x0 && ping.rect.x1 == alone.rect.x1); // same corner, same edge
            CHECK(std::fabs((ping.rect.y1 - ping.rect.y0) - (alone.rect.y1 - alone.rect.y0)) < 0.01f);
            CHECK(ping.pad_x == fps.pad_x && ping.rounding == fps.rounding);
            const float gap = corner >= 2 ? fps.rect.y0 - ping.rect.y1 : ping.rect.y0 - fps.rect.y1;
            CHECK(gap > 0.0f && std::fabs(gap - size * 0.4f) < 0.01f);
        }
    }

    // The keys and the panels are independent; Ping needs Online Play and is never saved by the game.
    const std::string window = ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "window.cpp");
    CHECK(window.find("} else if (hud == DcpPing::HudKey::Fps) {\n                settings.show_fps = !settings.show_fps;\n"
                      "            } else if (hud == DcpPing::HudKey::Ping) {\n"
                      "                settings.show_ping = !settings.show_ping;") != std::string::npos);
    CHECK(window.find("runtime_online_play_enabled() != 0") != std::string::npos);
    const std::string overlay = ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "dcp_overlay.cpp");
    CHECK(overlay.find("return DcpSettings::Get().show_ping && runtime_online_play_enabled() != 0;") != std::string::npos);
    CHECK(overlay.find("if (PingShown()) {\n        PingCounter();") != std::string::npos);
    CHECK(overlay.find("DrawOverlayPanel(PlaceOverlayPanel(label.c_str(), corner, &fps), label.c_str());") !=
          std::string::npos);
    const std::string video_settings = ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "dcp_settings.cpp");
    CHECK(video_settings.find("bool Save()") == std::string::npos); // the game saves the display mode alone

    // Off by default, saved by the launcher, read by the video core.
    const DcpSettings::Values game;
    CHECK(!game.show_ping);
    bool listed = false;
    for (const ManagedSetting& setting : ManagedSettings()) listed |= setting.key == kPingDisplayKey && setting.default_value == "0";
    CHECK(listed && std::string(kPingDisplayKey) == "show_ping" && !IsOnlineSetting(kPingDisplayKey));
    const fs::path file = base / "ping" / "dcp.ini";
    fs::remove_all(file.parent_path());
    {
        LauncherSettings settings(file);
        CHECK(!settings.GetBool(kPingDisplayKey, false));
        settings.SetBool(kPingDisplayKey, true);
        CHECK(settings.Save());
    }
    CHECK(LauncherSettings(file).GetBool(kPingDisplayKey, false));
    DcpSettings::Values read;
    CHECK(DcpSettings::Read(file.string(), read) && read.show_ping && !read.show_fps);

    const std::string page = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp");
    CHECK(page.find("ui.Toggle(\"Ping display\", kPingHelp, kPingDisplayKey, false, \"Off\", \"On\")") != std::string::npos);
    CHECK(OnlinePageSections(false, OnlineTab::kSettings, 0, false) == std::vector<OnlineSection>{OnlineSection::kOnlinePlay});

    // Show me in Lobby: off for a player without the setting, live for the launcher's link at once, saved like any other,
    // and Reset takes it back off.
    {
        LauncherSettings settings(file);
        CHECK(!OnlineConfigOf(settings).lobby_visible && !ReadOnlineConfig(file).lobby_visible);
        settings.SetBool(kLobbyVisibleKey, true);
        CHECK(OnlineConfigOf(settings).lobby_visible && !ReadOnlineConfig(file).lobby_visible);
        CHECK(settings.Save());
        CHECK(ReadOnlineConfig(file).lobby_visible);
        settings.ResetToDefaults();
        CHECK(!OnlineConfigOf(settings).lobby_visible);
    }
}

// PLAY with Online play on waits for one finished Connection test on the game's UDP port, whatever its result: saved at
// once, kept across launcher starts and "Reset all settings", forgotten when Online play is turned off, and asked again
// for another port. The name, the Ping display and friends do not count.
void TestConnectionTestGate(const fs::path& base) {
    const fs::path file = base / "connection-test" / "dcp.ini";
    fs::remove_all(file.parent_path());
    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    };
    set_env("DCP_UDP_PORT", nullptr);
    set_env("DCP_ONLINE", nullptr);
    const auto port_of = [](const trg::SettingsStore& settings) {
        net::OnlineConfig config = OnlineConfigOf(settings);
        net::apply_environment(config);
        return net::connection_test_port(config);
    };
    const auto needed = [&](const trg::SettingsStore& settings, bool testing = false) {
        return ConnectionTestNeeded(settings, settings.GetBool(kOnlineEnabledKey, false), testing, port_of(settings));
    };

    {
        LauncherSettings settings(file);
        CHECK(port_of(settings) == 3658);
        CHECK(!needed(settings) && !needed(settings, true));
        settings.SetBool(kOnlineEnabledKey, true);
        CHECK(needed(settings) && needed(settings, true));
        CHECK(settings.Save());
        CHECK(RecordConnectionTest(settings, port_of(settings)));
        CHECK(!needed(settings) && needed(settings, true));
    }
    CHECK(WrittenValue(ReadAll(file), kConnectionTestPortKey, "3658"));

    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(kOnlineEnabledKey, false) && !needed(settings));
        settings.Set(kOnlineNameKey, "Renamed");
        settings.SetBool(kPingDisplayKey, true);
        ForgetConnectionTestWhenOff(settings, true);
        CHECK(!needed(settings));
        settings.ResetToDefaults();
        CHECK(!needed(settings));

        set_env("DCP_UDP_PORT", "40002");
        CHECK(port_of(settings) == 40002 && needed(settings));
        set_env("DCP_UDP_PORT", nullptr);
        settings.Set(kOnlineUdpPortKey, "40001");
        CHECK(port_of(settings) == 40001 && needed(settings));
        settings.Set(kOnlineUdpPortKey, "0");
        CHECK(!needed(settings));

        settings.SetBool(kOnlineEnabledKey, false);
        ForgetConnectionTestWhenOff(settings, false);
        CHECK(!needed(settings) && settings.Get(kConnectionTestPortKey) == "0");
        settings.SetBool(kOnlineEnabledKey, true);
        CHECK(needed(settings));
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), kConnectionTestPortKey, "0"));
    CHECK(needed(LauncherSettings(file)));

    // A saved test next to a saved Off (Online play turned on, tested, closed without Save) does not count.
    WriteAll(file, "online_enabled = 0\nonline_connection_test_port = 3658\n");
    {
        LauncherSettings settings(file);
        ForgetConnectionTestWhenOff(settings, false);
        settings.SetBool(kOnlineEnabledKey, true);
        CHECK(needed(settings));
    }
    CHECK(!IsValidSetting(kConnectionTestPortKey, "70000") && IsValidSetting(kConnectionTestPortKey, "3658"));
    CHECK(IsOnlineSetting(kConnectionTestPortKey));

    // A finished test counts whatever its verdict, a run that never produced a report does not, and PLAY says why.
    const std::string page = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp");
    const size_t record = page.find("void OnlinePage::RecordFinishedTest(");
    const size_t record_end = page.find("\n}\n", record);
    CHECK(record != std::string::npos && record_end != std::string::npos);
    const std::string record_body = page.substr(record, record_end - record);
    CHECK(record_body.find("report") == std::string::npos && record_body.find("RecordConnectionTest(settings, port)") != std::string::npos);
    CHECK(page.find("test_ = {true, std::move(report), port};") != std::string::npos);
    CHECK(page.find("run_connection_test(config, identity, net::online_name(config.name), game, port)") != std::string::npos);
    const size_t caught = page.find("} catch (const std::exception& e) {");
    CHECK(caught != std::string::npos && page.find("return;", caught) < page.find("test_ = {true", caught));
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("online_page_->PlayNeedsTest(settings_)") != std::string::npos);
    CHECK(launcher.find("trg::PlayCheck{false, kConnectionTestRequired,") != std::string::npos);
    CHECK(std::string(kConnectionTestRequired).find("Connection test") != std::string::npos);

    // The firewall note is Windows only, and only under the running test.
    const char* windows = ConnectionTestRunningNote(true);
    CHECK(windows && std::string(windows).find("Allow access") != std::string::npos);
    CHECK(ConnectionTestRunningNote(false) == nullptr);
    CHECK(page.find("ConnectionTestRunningNote(OnWindows())") != std::string::npos &&
          page.find("bool OnWindows() { return net::host_platform() == \"Windows\"; }") != std::string::npos);
    CHECK(net::platform_name(true, "", "") == "Windows" && net::platform_name(false, "Valve", "Jupiter") != "Windows");
    CHECK(page.find("Allow access") == std::string::npos && launcher.find("Allow access") == std::string::npos);
}

// While PLAY waits for the Connection test its whole section is a gold panel with Run test as the primary button; a
// blocked PLAY sends the player there. Presentation only: the gate itself is TestConnectionTestGate's.
void TestConnectionTestPanel(const fs::path& base) {
    const fs::path file = base / "connection-test-panel" / "dcp.ini";
    fs::remove_all(file.parent_path());
    const auto needed = [](const trg::SettingsStore& settings, bool testing = false) {
        net::OnlineConfig config = OnlineConfigOf(settings);
        net::apply_environment(config);
        return ConnectionTestNeeded(settings, config.enabled, testing, net::connection_test_port(config));
    };

    LauncherSettings settings(file);
    settings.SetBool(kOnlineEnabledKey, true);
    const ConnectionTestLook pending = ConnectionTestLookOf(needed(settings));
    CHECK(pending.panel && pending.primary_button);
    CHECK(pending.required_text && std::string(pending.required_text) == "Connection test required");

    // A run that stopped without a report, or one still running, keeps the panel; a finished one removes it.
    CHECK(ConnectionTestLookOf(needed(settings, true)).panel);
    CHECK(settings.Save());
    CHECK(RecordConnectionTest(settings, 3658));
    const ConnectionTestLook done = ConnectionTestLookOf(needed(settings));
    CHECK(!done.panel && !done.primary_button && done.required_text == nullptr);
    CHECK(ConnectionTestLookOf(needed(settings, true)).panel);

    // Online play off: no Connection test section, no panel.
    ForgetConnectionTestWhenOff(settings, false);
    settings.SetBool(kOnlineEnabledKey, false);
    CHECK(!ConnectionTestLookOf(needed(settings)).panel);
    for (const OnlineSection section : OnlinePageSections(false, OnlineTab::kSettings, 0, false)) {
        CHECK(section != OnlineSection::kConnectionTest);
    }

    // PLAY's second question in a frame is the press; the emphasis is a short soft pulse.
    PlayAttemptDetector detector;
    CHECK(!detector.Pressed(10) && detector.Pressed(10) && !detector.Pressed(11) && !detector.Pressed(12) && detector.Pressed(12));
    CHECK(ConnectionTestEmphasis(-1.0) == 0.0f && ConnectionTestEmphasis(kConnectionTestEmphasisSeconds) == 0.0f);
    float peak = 0.0f;
    for (double t = 0.0; t < kConnectionTestEmphasisSeconds; t += 0.01) {
        const float e = ConnectionTestEmphasis(t);
        CHECK(e >= 0.0f && e <= 1.0f);
        peak = std::max(peak, e);
    }
    CHECK(peak > 0.7f && kConnectionTestEmphasisSeconds >= 2.0 && kConnectionTestEmphasisSeconds <= 3.0);

    const std::string page = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp");
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    const size_t row = page.find("bool OnlinePage::DrawRequiredTestRow(");
    const size_t section = page.find("void OnlinePage::DrawConnectionTest(");
    const size_t friends = page.find("void OnlinePage::DrawFriends(");
    CHECK(row != std::string::npos && section > row && friends > section);
    if (row == std::string::npos || section <= row || friends <= section) return;
    const std::string required_row = page.substr(row, section - row);
    const std::string draw = page.substr(section, friends - section);

    // Blocked PLAY: back to the Online page as before, and the next frame scrolls to the section and focuses Run test.
    const size_t blocked = launcher.find("online_page_->PlayNeedsTest(settings_)) {");
    CHECK(blocked != std::string::npos);
    const std::string gate = launcher.substr(blocked, launcher.find("return trg::PlayCheck{};", blocked) - blocked);
    CHECK(gate.find("play_attempts_.Pressed(ImGui::GetFrameCount())") != std::string::npos);
    CHECK(gate.find("PointToConnectionTest()") != std::string::npos);
    CHECK(gate.find("trg::PlayCheck{false, kConnectionTestRequired,") != std::string::npos);
    CHECK(draw.find("ImGui::SetScrollHereY(") != std::string::npos && draw.find("pointed_at_ = ImGui::GetTime()") != std::string::npos);
    const size_t focus = required_row.find("ImGui::SetKeyboardFocusHere()");
    const size_t primary = required_row.find("ui.AccentButton(button)");
    CHECK(focus != std::string::npos && primary != std::string::npos && focus < primary);
    CHECK(draw.find("ImGuiChildFlags_NavFlattened") != std::string::npos);

    // The panel reuses the friend request card's drawing, in the accent colour and never the warning red.
    CHECK(page.find("BeginCard(ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f), ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f), 1.0f);") != std::string::npos);
    CHECK(page.find("ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);") != std::string::npos);
    CHECK(page.find("ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 10.0f * s));") != std::string::npos);
    CHECK(draw.find("BeginCard(background, border,") != std::string::npos);
    CHECK(draw.substr(0, draw.find("TestResult result;")).find("warn_dot") == std::string::npos &&
          required_row.find("warn_dot") == std::string::npos);
    CHECK(required_row.find("ui.theme().accent") != std::string::npos);
    CHECK(draw.find("ui.ButtonRow(\"Connection test\", kTestHelp, button)") != std::string::npos);
    CHECK(launcher.find("kConnectionTestRequired") != std::string::npos);
}

// The launcher's lobby link through a running development lobby (DCP_TEST_LOBBY): presence while a launcher is open,
// requests and answers from events without any refresh, several sessions of one account, a rename, and Off.
void TestOnlineClient(const fs::path& base) {
    const char* lobby = std::getenv("DCP_TEST_LOBBY");
    if (!lobby || !*lobby) {
        std::printf("online client tests skipped (set DCP_TEST_LOBBY to a running development lobby)\n");
        return;
    }
    const fs::path dir = base / "online-client";
    fs::remove_all(dir);
    const auto clock = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    struct Seat {
        OnlineClient client;
        net::OnlineConfig config;
        bool on = true;
        Seat(const fs::path& user, const char* lobby, const char* name) : client(user) {
            config.enabled = true;
            config.lobby = lobby;
            config.name = name;
        }
    };
    Seat alice(dir / "a", lobby, "Alice"), bea(dir / "b", lobby, "Bea"), bea_game(dir / "b", lobby, "Bea"),
        cleo(dir / "c", lobby, "Cleo");
    bea_game.on = cleo.on = false;
    const auto pump = [&](const std::function<bool()>& done, double seconds = 6.0) {
        const double end = clock() + seconds;
        for (;;) {
            for (Seat* seat : {&alice, &bea, &bea_game, &cleo}) {
                seat->client.WatchLobby(clock());
                seat->client.Update(seat->on, seat->config, "CUSA10454-1.00", clock());
            }
            if (done()) return true;
            if (clock() >= end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    const auto& a = alice.client.state();
    const auto& b = bea.client.state();
    const auto first_friend_online = [&](const OnlineClient::State& s) { return !s.friends.empty() && s.friends[0].online; };

    CHECK(pump([&] { return a.link == OnlineClient::Link::kUp && b.link == OnlineClient::Link::kUp; }));
    CHECK(!a.friend_code.empty() && !b.friend_code.empty() && a.friend_code != b.friend_code);
    const std::string code_b = b.friend_code;

    bea.client.AddFriend(a.friend_code);
    CHECK(pump([&] { return a.incoming.size() == 1 && b.outgoing.size() == 1; }));
    CHECK(a.incoming.size() == 1 && a.incoming[0].name == "Bea" && b.notice == "Request sent to Alice.");
    const std::string account_b = a.incoming.empty() ? "" : a.incoming[0].account;

    alice.client.Accept(account_b);
    CHECK(a.incoming.empty() && a.friends.size() == 1 && a.friends[0].name == "Bea");
    CHECK(pump([&] { return first_friend_online(a) && first_friend_online(b) && b.outgoing.empty(); }));
    CHECK(!a.friends.empty() && a.friends[0].platform == net::host_platform() && a.friends[0].version == net::port_version() &&
          a.friends[0].online_protocol == net::kOnlineProtocolVersion);

    bea_game.on = true;
    CHECK(pump([&] { return bea_game.client.state().link == OnlineClient::Link::kUp; }));
    bea.on = false;
    CHECK(!pump([&] { return !first_friend_online(a); }, 1.5));
    bea_game.on = false;
    CHECK(pump([&] { return !a.friends.empty() && !a.friends[0].online; }));

    bea.on = true;
    CHECK(pump([&] { return first_friend_online(a); }));
    bea.config.name = "Beatrix";
    CHECK(pump([&] { return !a.friends.empty() && a.friends[0].name == "Beatrix" && a.friends[0].online; }, 10.0));
    CHECK(b.friend_code == code_b && a.friends.size() == 1 && a.friends[0].account == account_b);

    cleo.on = true;
    CHECK(pump([&] { return cleo.client.state().link == OnlineClient::Link::kUp; }));
    cleo.client.AddFriend(a.friend_code);
    CHECK(pump([&] { return a.incoming.size() == 1; }));
    alice.client.Decline(a.incoming.empty() ? "" : a.incoming[0].account);
    CHECK(a.incoming.empty());
    CHECK(!pump([&] { return !a.incoming.empty(); }, 1.0));

    // The public Lobby: Bea never asked, so she is not listed; a row goes Pending at once and Friend after the accept, from
    // the friend events; and leaving takes the player off the list.
    const OnlineClient::State& c = cleo.client.state();
    const auto lists = [](const OnlineClient::State& s, const char* name, LobbyRelation relation) {
        return std::any_of(s.lobby.begin(), s.lobby.end(),
                           [&](const LobbyPlayer& p) { return p.name == name && p.relation == relation; });
    };
    alice.config.lobby_visible = cleo.config.lobby_visible = true;
    CHECK(pump([&] { return lists(a, "Cleo", LobbyRelation::kNone) && lists(c, "Alice", LobbyRelation::kNone); }));
    CHECK(a.lobby.size() == 1 && c.lobby.size() == 1 && b.lobby.empty() && !b.lobby_fetched);
    cleo.client.RequestFromLobby(c.lobby.empty() ? "" : c.lobby[0].player);
    CHECK(lists(c, "Alice", LobbyRelation::kPending));
    CHECK(pump([&] { return a.incoming.size() == 1 && lists(a, "Cleo", LobbyRelation::kIncoming); }));
    alice.client.DeclineFromLobby(a.lobby.empty() ? "" : a.lobby[0].player);
    CHECK(lists(a, "Cleo", LobbyRelation::kNone));
    CHECK(pump([&] { return a.incoming.empty() && lists(c, "Alice", LobbyRelation::kNone); }, 35.0));
    cleo.client.RequestFromLobby(c.lobby.empty() ? "" : c.lobby[0].player);
    CHECK(pump([&] { return a.incoming.size() == 1 && lists(a, "Cleo", LobbyRelation::kIncoming); }));
    alice.client.RequestFromLobby(a.lobby.empty() ? "" : a.lobby[0].player);
    CHECK(lists(a, "Cleo", LobbyRelation::kFriend));
    CHECK(pump([&] { return lists(c, "Alice", LobbyRelation::kFriend) && a.incoming.empty() && a.friends.size() == 2; }));
    cleo.config.lobby_visible = false;
    CHECK(pump([&] {
        alice.client.RefreshLobby(clock());
        return a.lobby_fetched && a.lobby.empty();
    }, LobbyRefresh::kManualGapSeconds + 5.0));
    CHECK(c.lobby.empty() && !c.lobby_fetched);

    alice.on = false;
    pump([] { return true; });
    CHECK(a.link == OnlineClient::Link::kOff && a.friend_code.empty() && a.friends.empty() && a.incoming.empty());
    CHECK(pump([&] { return !b.friends.empty() && !b.friends[0].online; }));
}

void TestSkipIntroSettings(const fs::path& base) {
    const fs::path file = base / "skip_intro" / "dcp.ini";
    fs::remove_all(file.parent_path());
    int managed = 0;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed += setting.key == kSkipIntroKey && setting.default_value == "0";
    }
    CHECK(managed == 1);
    CHECK(dcp_skip_intro(nullptr) == 0 && dcp_skip_intro(file.string().c_str()) == 0);
    WriteAll(file, "vsync = 1\n");
    CHECK(dcp_skip_intro(file.string().c_str()) == 0);
    WriteAll(file, "skip_intro = 1\n");
    CHECK(dcp_skip_intro(file.string().c_str()) == 1);
    WriteAll(file, "skip_intro = banana\n");
    CHECK(dcp_skip_intro(file.string().c_str()) == 0);
    {
        LauncherSettings settings(file);
        settings.SetBool(kSkipIntroKey, true);
        CHECK(settings.Save());
        CHECK(dcp_skip_intro(file.string().c_str()) == 1);
        settings.ResetToDefaults();
        CHECK(!settings.GetBool(kSkipIntroKey, true));
    }
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    const std::string toggle = "ui.Toggle(\"Skip Intro\", kSkipIntroHelp, kSkipIntroKey, false, \"Off\", \"On\");";
    const size_t row = launcher.find(toggle);
    CHECK(row != std::string::npos && launcher.find(toggle, row + 1) == std::string::npos);
    CHECK(row < launcher.find("ui.Combo(\"Frame rate\"") && row > launcher.find("ui.Toggle(\"Japanese NPC Voices\""));
    const std::string loader = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c");
    CHECK(loader.find("skip_intro_install(image,size,entry,known_build ? build_key : NULL,skip_intro_requested,") != std::string::npos);
    CHECK(loader.find("skip_intro_wanted(dcp_skip_intro(settings_file))") != std::string::npos);
}

// The one gallery option: Off by default, in the Gameplay page after the skip offers, offered only where the Gallery
// list builder is validated, and no other gallery switch anywhere in the product.
void TestEarlyStoryOnlineSetting(const fs::path& base) {
    using namespace localization;
    CHECK(std::string(kEarlyStoryOnlineKey) == "early_story_online" && !IsOnlineSetting(kEarlyStoryOnlineKey));
    int managed = 0;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed += setting.key == kEarlyStoryOnlineKey && setting.default_value == "0";
    }
    CHECK(managed == 1 && IsValidSetting(kEarlyStoryOnlineKey, "0") && IsValidSetting(kEarlyStoryOnlineKey, "1"));

    const fs::path file = base / "early_story_online" / "dcp.ini";
    fs::remove_all(file.parent_path());
    {
        LauncherSettings settings(file);
        CHECK(!settings.GetBool(kEarlyStoryOnlineKey, false));
        settings.SetBool(kEarlyStoryOnlineKey, true);
        CHECK(settings.Save());
    }
    CHECK(LauncherSettings(file).GetBool(kEarlyStoryOnlineKey, false));
    {
        LauncherSettings settings(file);
        settings.ResetToDefaults();
        CHECK(!settings.GetBool(kEarlyStoryOnlineKey, true));
    }

    const std::string page = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp");
    CHECK(page.find("ui.Toggle(\"Early Story Online\", kEarlyStoryHelp, kEarlyStoryOnlineKey, false, \"Off\", \"On\");") !=
          std::string::npos);
    const std::string help =
        "Lets you host online play right after the tutorial. The tutorial remains single-player. Only the host needs this "
        "option; guests keep their own campaign progress. Early story rooms are friend/direct only.";
    CHECK(page.find("\"Lets you host online play right after the tutorial. The tutorial remains single-player. Only the "
                    "host needs this \"") != std::string::npos &&
          page.find("\"option; guests keep their own campaign progress. Early story rooms are friend/direct only.\";") !=
              std::string::npos);

    using S = OnlineSection;
    using T = OnlineTab;
    for (const T tab : kOnlineTabs) {
        const std::vector<S> off = OnlinePageSections(false, tab, 1, true);
        CHECK(std::find(off.begin(), off.end(), S::kEarlyStoryOnline) == off.end());
    }
    const std::vector<S> settings_tab = OnlinePageSections(true, T::kSettings, 0, false);
    const auto at = std::find(settings_tab.begin(), settings_tab.end(), S::kEarlyStoryOnline);
    CHECK(at != settings_tab.end() && at != settings_tab.begin() && *(at - 1) == S::kPingDisplay);
    for (const T tab : {T::kFriends, T::kLobby}) {
        const std::vector<S> other = OnlinePageSections(true, tab, 2, true);
        CHECK(std::find(other.begin(), other.end(), S::kEarlyStoryOnline) == other.end());
    }

    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    };
    const char* previous_raw = std::getenv("DCP_EARLY_STORY_ONLINE");
    const bool had_previous = previous_raw != nullptr;
    const std::string previous = previous_raw ? previous_raw : "";
    const std::string path = file.string();
    set_env("DCP_EARLY_STORY_ONLINE", nullptr);
    CHECK(dcp_early_story_online(nullptr) == 0);
    WriteAll(file, "vsync = 1\n");
    CHECK(dcp_early_story_online(path.c_str()) == 0);
    WriteAll(file, "early_story_online = banana\n");
    CHECK(dcp_early_story_online(path.c_str()) == 0);
    WriteAll(file, "early_story_online = 1\n");
    CHECK(dcp_early_story_online(path.c_str()) == 1);
    for (const char* off : {"0", "off", "false"}) {
        set_env("DCP_EARLY_STORY_ONLINE", off);
        CHECK(dcp_early_story_online(path.c_str()) == 0);
    }
    set_env("DCP_EARLY_STORY_ONLINE", "maybe");
    CHECK(dcp_early_story_online(path.c_str()) == 1);
    WriteAll(file, "early_story_online = 0\n");
    CHECK(dcp_early_story_online(path.c_str()) == 0);
    for (const char* on : {"1", "on", "true"}) {
        set_env("DCP_EARLY_STORY_ONLINE", on);
        CHECK(dcp_early_story_online(path.c_str()) == 1);
        CHECK(dcp_early_story_online(nullptr) == 1);
    }
    set_env("DCP_EARLY_STORY_ONLINE", had_previous ? previous.c_str() : nullptr);

    int translated = 0;
    for (const UiTextRow& row : PortugueseBrazilUiText().rows) {
        if (row.english == "Early Story Online" || row.english == help) {
            ++translated;
            CHECK(!row.text.empty() && row.text != row.english);
        }
    }
    CHECK(translated == 2);

    for (const char* menu : {"dcp_options_menu.cpp", "dcp_options_menu.h"}) {
        const fs::path source = fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / menu;
        if (fs::exists(source)) {
            CHECK(ReadAll(source).find("early_story_online") == std::string::npos);
        }
    }
}

void TestUnlockGallerySettings(const fs::path& base) {
    const fs::path file = base / "unlock_gallery" / "dcp.ini";
    fs::remove_all(file.parent_path());
    int managed = 0;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed += setting.key == kUnlockGalleryKey && setting.default_value == "0";
    }
    CHECK(managed == 1 && std::string(kUnlockGalleryKey) == "unlock_gallery");
    CHECK(dcp_unlock_gallery(nullptr) == 0 && dcp_unlock_gallery(file.string().c_str()) == 0);
    WriteAll(file, "vsync = 1\nskip_intro = 1\n");
    CHECK(dcp_unlock_gallery(file.string().c_str()) == 0);
    WriteAll(file, "unlock_gallery = 1\n");
    CHECK(dcp_unlock_gallery(file.string().c_str()) == 1);
    WriteAll(file, "unlock_gallery = banana\n");
    CHECK(dcp_unlock_gallery(file.string().c_str()) == 0);
    {
        LauncherSettings settings(file);
        settings.SetBool(kUnlockGalleryKey, true);
        CHECK(settings.Save());
        CHECK(dcp_unlock_gallery(file.string().c_str()) == 1);
        CHECK(!settings.GetBool(kSkipIntroKey, false) && !settings.GetBool(kStorySkipOfferKey, false));
        settings.ResetToDefaults();
        CHECK(!settings.GetBool(kUnlockGalleryKey, true));
    }

    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(build.unlock_gallery && UnlockGalleryAvailable(&build));
    }
    GameBuild without = SupportedBuilds()[0];
    without.unlock_gallery = false;
    CHECK(!UnlockGalleryAvailable(&without) && UnlockGalleryAvailable(nullptr));
    CHECK(SupportedBuilds().size() == 8);

    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    const std::string gated = "if (UnlockGalleryAvailable(build_)) {\n            ui.Toggle(\"Unlock Gallery\", kUnlockGalleryHelp, "
                              "kUnlockGalleryKey, false, \"Off\", \"On\");";
    const size_t row = launcher.find(gated);
    CHECK(row != std::string::npos && launcher.find("ui.Toggle(\"Unlock Gallery\"") == launcher.rfind("ui.Toggle(\"Unlock Gallery\""));
    const size_t page = launcher.find("void PageGameplay(trg::Ui& ui) {");
    CHECK(page != std::string::npos && row > launcher.find("ui.Toggle(\"Tutorial Skip Offer\"", page) &&
          row < launcher.find("ui.Toggle(\"Allow overlay programs\"", page));
    CHECK(launcher.find("kUnlockGalleryHelp[] = \"Unlocks all Gallery content while enabled without changing save progress\";") !=
          std::string::npos);

    const std::string loader = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c");
    CHECK(loader.find("gallery_unlock_wanted(dcp_unlock_gallery(settings_file))") != std::string::npos);
    CHECK(loader.find("gallery_unlock_install(image,size,entry,known_build && game_build.unlock_gallery ? build_key : NULL,") !=
          std::string::npos);
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "game_files.cpp").find("out->unlock_gallery = build->unlock_gallery;") !=
          std::string::npos);

    // There is one gallery option: no second spelling of it in the product, its documentation or its tools.
    const fs::path source(DCP_SOURCE_DIR);
    int scanned = 0;
    for (const char* dir : {"src", "launcher", "gpu/shim", "docs", "tools", "scripts"}) {
        for (const auto& entry : fs::recursive_directory_iterator(source / dir)) {
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file() || (ext != ".c" && ext != ".cpp" && ext != ".h" && ext != ".md" && ext != ".py" &&
                                             ext != ".ps1" && ext != ".sh" && ext != ".txt")) {
                continue;
            }
            std::string text = ReadAll(entry.path());
            for (char& c : text) c = (char)std::tolower((unsigned char)c);
            ++scanned;
            for (const char* name : {"full gallery", "full_gallery", "gallery_full", "unlock_all_gallery", "unlock full gallery",
                                     "complete gallery", "gallery 100", "gallery unlock all", "gallery_unlock_all"}) {
                if (text.find(name) != std::string::npos) std::printf("second gallery option in %s\n", entry.path().string().c_str());
                CHECK(text.find(name) == std::string::npos);
            }
        }
    }
    CHECK(scanned > 100);
    std::string readme = ReadAll(source / "README.md");
    for (char& c : readme) c = (char)std::tolower((unsigned char)c);
    CHECK(readme.find("full gallery") == std::string::npos && readme.find("full_gallery") == std::string::npos);
}

void TestJapaneseMenuButtonsSettings(const fs::path& base) {
    const fs::path file = base / "menu_buttons" / "dcp.ini";
    fs::remove_all(file.parent_path());
    int managed = 0;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed += setting.key == kJapaneseMenuButtonsKey && setting.default_value == "0";
    }
    CHECK(managed == 1 && std::string(kJapaneseMenuButtonsKey) == "japanese_menu_buttons");
    CHECK(dcp_japanese_menu_buttons(nullptr) == 0 && dcp_japanese_menu_buttons(file.string().c_str()) == 0);
    WriteAll(file, "vsync = 1\ntext_language = ja\n");
    CHECK(dcp_japanese_menu_buttons(file.string().c_str()) == 0);
    WriteAll(file, "japanese_menu_buttons = 1\n");
    CHECK(dcp_japanese_menu_buttons(file.string().c_str()) == 1);
    WriteAll(file, "japanese_menu_buttons = banana\n");
    CHECK(dcp_japanese_menu_buttons(file.string().c_str()) == 0);
    {
        LauncherSettings settings(file);
        settings.SetBool(kJapaneseMenuButtonsKey, true);
        CHECK(settings.Save());
        CHECK(dcp_japanese_menu_buttons(file.string().c_str()) == 1);
        CHECK(!settings.GetBool(kSkipIntroKey, false));
        settings.ResetToDefaults();
        CHECK(!settings.GetBool(kJapaneseMenuButtonsKey, true));
    }
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    const std::string toggle =
        "ui.Toggle(\"Japanese Menu Buttons\", JapaneseMenuButtonsHelp().c_str(), kJapaneseMenuButtonsKey, false, \"Off\", \"On\");";
    const size_t row = launcher.find(toggle);
    CHECK(row != std::string::npos && launcher.find(toggle, row + 1) == std::string::npos);
    const size_t controls = launcher.find("void PageControls(trg::Ui& ui) {");
    CHECK(controls != std::string::npos && row > launcher.find("ControllerPromptsCombo(ui);", controls) &&
          row < launcher.find("ui.Info(\"Keyboard\"", controls));
    CHECK(launcher.find("ui.Toggle(\"Skip Intro\"") < controls && row > controls);
    const size_t help = launcher.find("std::string JapaneseMenuButtonsHelp() const {");
    CHECK(help != std::string::npos && launcher.find("prompts_installed_ == PromptStyle::kXbox", help) < help + 200);
    CHECK(launcher.find("\"Off: %s confirms and %s cancels in menus.") != std::string::npos);
    CHECK(launcher.find("xbox ? \"A\" : \"Cross\"", help) != std::string::npos &&
          launcher.find("xbox ? \"B\" : \"Circle\"", help) != std::string::npos);
    const std::string loader = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c");
    CHECK(loader.find("menu_buttons_wanted(dcp_japanese_menu_buttons(settings_file))") != std::string::npos);
    CHECK(loader.find("menu_buttons_install(image,size,known_build ? build_key : NULL,japanese_menu_buttons)") != std::string::npos);
    CHECK(loader.find("exit_game_set_accept_east(japanese_menu_buttons && menu_buttons_applied)") != std::string::npos);
}

void TestControllerRumbleSettings(const fs::path& base) {
    const fs::path file = base / "controller_rumble" / "dcp.ini";
    fs::remove_all(file.parent_path());
    int managed = 0;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed += setting.key == kControllerRumbleKey && setting.default_value == "1";
    }
    CHECK(managed == 1 && std::string(kControllerRumbleKey) == "controller_rumble");
    CHECK(dcp_controller_rumble(nullptr) == 1 && dcp_controller_rumble(file.string().c_str()) == 1);
    WriteAll(file, "vsync = 1\n");
    CHECK(dcp_controller_rumble(file.string().c_str()) == 1);
    WriteAll(file, "controller_rumble = 1\n");
    CHECK(dcp_controller_rumble(file.string().c_str()) == 1);
    WriteAll(file, "controller_rumble = 0\n");
    CHECK(dcp_controller_rumble(file.string().c_str()) == 0);
    WriteAll(file, "controller_rumble = banana\n");
    CHECK(dcp_controller_rumble(file.string().c_str()) == 1);
    {
        LauncherSettings settings(file);
        settings.SetBool(kControllerRumbleKey, false);
        CHECK(settings.Save());
        CHECK(dcp_controller_rumble(file.string().c_str()) == 0);
        settings.ResetToDefaults();
        CHECK(settings.GetBool(kControllerRumbleKey, false));
    }
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    const std::string toggle = "ui.Toggle(\"Controller Rumble\", kControllerRumbleHelp, kControllerRumbleKey, true, \"Off\", \"On\");";
    const size_t row = launcher.find(toggle);
    const size_t controls = launcher.find("void PageControls(trg::Ui& ui) {");
    CHECK(row != std::string::npos && launcher.find(toggle, row + 1) == std::string::npos);
    CHECK(row > launcher.find("ui.Toggle(\"Japanese Menu Buttons\"", controls) && row < launcher.find("ui.Info(\"Keyboard\"", controls));

    const std::string loader = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c");
    CHECK(loader.find("runtime_pad_set_rumble(dcp_controller_rumble(settings_file));") != std::string::npos);
    const std::string pad = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "runtime_pad.c");
    const size_t vibration = pad.find("static ABI int32_t pad_vibration(");
    const size_t end = pad.find("return 0;", vibration);
    CHECK(vibration != std::string::npos && pad.find("if (g && dcpgpu_controller_rumble()) SDL_RumbleGamepad(", vibration) < end);
    CHECK(pad.find("slots[slot].gamepad", vibration) < end);
    CHECK(pad.find("if (slot<0) return ERR_INVALID_HANDLE;", vibration) < pad.find("dcpgpu_controller_rumble()", vibration));

    const std::string portuguese = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "localization" / "pt_br_ui_text.cpp");
    CHECK(portuguese.find("Controller Rumble\tVibração do controle\n") != std::string::npos);
}

void TestSkipOfferSettings(const fs::path& base) {
    const fs::path file = base / "skip_offers" / "dcp.ini";
    fs::remove_all(file.parent_path());
    int managed = 0;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed += (setting.key == kStorySkipOfferKey || setting.key == kTutorialSkipOfferKey) && setting.default_value == "0";
    }
    CHECK(managed == 2);
    CHECK(dcp_skip_offers(nullptr) == 0 && dcp_skip_offers(file.string().c_str()) == 0);
    WriteAll(file, "vsync = 1\n");
    CHECK(dcp_skip_offers(file.string().c_str()) == 0);
    WriteAll(file, "story_skip_offer = 0\ntutorial_skip_offer = 1\n");
    CHECK(dcp_skip_offers(file.string().c_str()) == 2);
    WriteAll(file, "story_skip_offer = 1\ntutorial_skip_offer = 0\n");
    CHECK(dcp_skip_offers(file.string().c_str()) == 1);
    {
        LauncherSettings settings(file);
        settings.SetBool(kStorySkipOfferKey, true);
        settings.SetBool(kTutorialSkipOfferKey, true);
        CHECK(settings.Save());
    }
    CHECK(dcp_skip_offers(file.string().c_str()) == 3);
    // Test instances turn both on only when the caller has not set them.
    const std::string instance = ReadAll(fs::path(DCP_SOURCE_DIR) / "tools" / "online-instance.ps1");
    CHECK(instance.find("if (-not $env:DCP_STORY_SKIP) { $env:DCP_STORY_SKIP = '1' }") != std::string::npos);
    CHECK(instance.find("if (-not $env:DCP_TUTORIAL_SKIP) { $env:DCP_TUTORIAL_SKIP = '1' }") != std::string::npos);
    for (const char* writes : {"Set-Content", "Add-Content", "Out-File"}) CHECK(instance.find(writes) == std::string::npos);
    // Both switches are shown for every build; the loader passes every supported build's key to the skip offers.
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("SkipOffersAvailable") == std::string::npos);
    CHECK(launcher.find("        ui.Toggle(\"Story Skip Offer\", kStorySkipOfferHelp,") != std::string::npos);
    const std::string loader = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c");
    CHECK(loader.find("skip_offers_install(image,size,known_build ? build_key : NULL,") != std::string::npos);
}

// A test instance pinned with DCP_GAME_BUILD (tools\online-instance.ps1 -Build) uses that build's folders and never
// the active build; the script's build table is the launcher's.
void TestPinnedBuild(const fs::path& base) {
    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    };
    const std::string script = ReadAll(fs::path(DCP_SOURCE_DIR) / "tools" / "online-instance.ps1");
    size_t listed = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        const std::string row = "'" + BuildKey(build) + "' = @{ Folder = '" + build.folder + "';";
        const size_t at = script.find(row);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            const size_t end = script.find('\n', at);
            CHECK(script.substr(at, end - at).find("Label = '" + std::string(build.label) + "' }") != std::string::npos);
            ++listed;
        }
    }
    size_t rows = 0;
    for (size_t at = script.find("= @{ Folder = '"); at != std::string::npos; at = script.find("= @{ Folder = '", at + 1)) ++rows;
    CHECK(listed == SupportedBuilds().size() && rows == listed);
    CHECK(BuildKey(kUs) == "CUSA10454-1.00" && BuildKey(kCk100) == "CUSA10209-1.00");
    CHECK(script.find("if ($pinned) { $env:DCP_GAME_BUILD = $pinned.Key } else { Remove-Item Env:DCP_GAME_BUILD") !=
          std::string::npos);
    CHECK(script.find("[Environment]::SetEnvironmentVariable($variable, $previous[$variable], 'Process')") != std::string::npos);
    for (const char* scope : {"'User')", "'Machine')", "setx"}) CHECK(script.find(scope) == std::string::npos);

    // A build tree with the US and Asia/Hong Kong 1.00 builds complete, Japan 1.02 a core preparation only.
    const fs::path root = base / "pinned";
    fs::remove_all(root);
    const fs::path exe_dir = root / "build" / "cmake" / "clang64" / "bin";
    const GameRoots roots{root / "game" / "prepared", root / "build" / "image"};
    const auto prepare = [&](const GameBuild& build, const char* scope) {
        const GamePaths paths = BuildPaths(roots, build);
        WriteAll(paths.app0 / kPreparedMarker, std::string("{\n  \"version\": \"x\",\n  \"scope\": \"") + scope + "\"\n}\n");
        WriteAll(paths.image_dir / "boot.bin", "boot");
    };
    prepare(kUs, "full");
    prepare(kCk100, "full");
    prepare(kJp102, "core");
    const auto marker_time = [&](const GameBuild& build) {
        return fs::last_write_time(BuildPaths(roots, build).app0 / kPreparedMarker);
    };
    fs::last_write_time(BuildPaths(roots, kUs).app0 / kPreparedMarker, marker_time(kCk100) - std::chrono::hours(1));
    const auto us_time = marker_time(kUs), ck_time = marker_time(kCk100);

    CHECK(!PinnedBuildOf(roots, "").set);
    const PinnedBuild us = PinnedBuildOf(roots, "CUSA10454-1.00"), ck = PinnedBuildOf(roots, "CUSA10209-1.00");
    CHECK(us.set && us.build == &kUs && us.error.empty() && ck.build == &kCk100);
    const PinnedBuild core = PinnedBuildOf(roots, "CUSA07001-1.02");
    CHECK(core.set && !core.build && core.error.find("Build CUSA07001-1.02 is not prepared") == 0);
    const PinnedBuild absent = PinnedBuildOf(roots, "CUSA10454-1.01");
    CHECK(absent.set && !absent.build && absent.error.find("Build CUSA10454-1.01 is not prepared") == 0);
    const PinnedBuild unknown = PinnedBuildOf(roots, "CUSA99999-1.00");
    CHECK(unknown.set && !unknown.build && unknown.error.find("DCP_GAME_BUILD names no supported build") == 0);

    // The active build is the newest marker (Asia/Hong Kong here); each pinned instance keeps its own build.
    set_env(kPinnedBuildVariable, nullptr);
    CHECK(ResolvePaths({}, exe_dir).app0 == BuildPaths(roots, kCk100).app0);
    set_env(kPinnedBuildVariable, "CUSA10454-1.00");
    const Paths a = ResolvePaths({}, exe_dir);
    CHECK(a.error.empty() && a.app0 == BuildPaths(roots, kUs).app0 && a.image_dir == BuildPaths(roots, kUs).image_dir);
    CHECK(a.boot == BuildPaths(roots, kUs).image_dir / "boot.bin");
    set_env(kPinnedBuildVariable, "CUSA10209-1.00");
    const Paths b = ResolvePaths({}, exe_dir);
    CHECK(b.error.empty() && b.app0 == BuildPaths(roots, kCk100).app0 && b.app0 != a.app0);
    // Another instance activating a build (its marker newest) moves neither pin.
    fs::last_write_time(BuildPaths(roots, kUs).app0 / kPreparedMarker, ck_time + std::chrono::hours(1));
    CHECK(ResolvePaths({}, exe_dir).app0 == BuildPaths(roots, kCk100).app0);
    set_env(kPinnedBuildVariable, "CUSA10454-1.00");
    fs::last_write_time(BuildPaths(roots, kCk100).app0 / kPreparedMarker, ck_time + std::chrono::hours(2));
    CHECK(ResolvePaths({}, exe_dir).app0 == BuildPaths(roots, kUs).app0);
    fs::last_write_time(BuildPaths(roots, kUs).app0 / kPreparedMarker, us_time);
    fs::last_write_time(BuildPaths(roots, kCk100).app0 / kPreparedMarker, ck_time);
    // Explicit paths still win; a pin that cannot be used is an error, never the active build.
    PathRequest explicit_app0;
    explicit_app0.app0 = Utf8(root / "elsewhere");
    CHECK(ResolvePaths(explicit_app0, exe_dir).app0 == root / "elsewhere");
    set_env(kPinnedBuildVariable, "CUSA07001-1.02");
    CHECK(ResolvePaths({}, exe_dir).error.find("Build CUSA07001-1.02 is not prepared") == 0);
    set_env(kPinnedBuildVariable, "CUSA99999-1.00");
    CHECK(!ResolvePaths({}, exe_dir).error.empty());
    set_env(kPinnedBuildVariable, nullptr);
    CHECK(ResolvePaths({}, exe_dir).error.empty() && ResolvePaths({}, exe_dir).app0 == BuildPaths(roots, kCk100).app0);
    // Resolving never writes a marker.
    CHECK(marker_time(kUs) == us_time && marker_time(kCk100) == ck_time);

    // The pinned launcher refuses package selection, which would activate a build for every instance; the game
    // refuses an image of another build.
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("if (picked.empty() || prompts_.running() || BuildPinned()) {") != std::string::npos);
    CHECK(launcher.find("browser_.active() || prompts_.running() || BuildPinned()) {") != std::string::npos);
    const std::string loader = ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c");
    CHECK(loader.find("\"DCP_GAME_BUILD is %s but the game image is %s\"") != std::string::npos);
    CHECK(loader.find("fail(paths.error[0] ? paths.error : \"cannot locate the game folders\");") != std::string::npos);
    fs::remove_all(root);
}

void TestOverlayProgramsSetting(const fs::path& base) {
    const fs::path file = base / "overlays" / "dcp.ini";
    fs::remove_all(file.parent_path());
    bool listed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (setting.key == kAllowOverlayProgramsKey) {
            listed = true;
            CHECK(setting.default_value == "0");
        }
    }
    CHECK(listed);
    {
        LauncherSettings settings(file);
        CHECK(!settings.GetBool(kAllowOverlayProgramsKey, false));
        settings.SetBool(kAllowOverlayProgramsKey, true);
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), kAllowOverlayProgramsKey, "1"));
    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(kAllowOverlayProgramsKey, false));
        settings.ResetToDefaults();
        CHECK(!settings.GetBool(kAllowOverlayProgramsKey, true));
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), kAllowOverlayProgramsKey, "0"));

    // Only On, and only where the separate process exists, leaves the launcher's process.
    CHECK(!SeparateGameProcessWanted(false, true));
    CHECK(SeparateGameProcessWanted(true, true));
    CHECK(!SeparateGameProcessWanted(false, false));
    CHECK(!SeparateGameProcessWanted(true, false));
#ifdef _WIN32
    CHECK(kSeparateGameProcessAvailable);
#else
    CHECK(!kSeparateGameProcessAvailable);
#endif

    // Neither mode filters Vulkan layers or closes extension points, so overlays and a player's own
    // VK_LOADER_LAYERS_DISABLE reach the game untouched.
    const fs::path source(DCP_SOURCE_DIR);
    for (const char* dir : {"src", "launcher"}) {
        for (const auto& entry : fs::recursive_directory_iterator(source / dir)) {
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file() || (ext != ".c" && ext != ".cpp" && ext != ".h")) {
                continue;
            }
            const std::string text = ReadAll(entry.path());
            for (const char* absent : {"VK_LOADER_LAYERS_DISABLE", "ProcessExtensionPointDisablePolicy",
                                       "EXTENSION_POINT_DISABLE", "PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY"}) {
                if (text.find(absent) != std::string::npos) {
                    std::printf("%s in %s\n", absent, entry.path().string().c_str());
                    CHECK(false);
                }
            }
        }
    }
    CHECK(!fs::exists(source / "launcher" / "overlay_protection.cpp"));
    CHECK(!fs::exists(source / "launcher" / "vulkan_layers.cpp"));
}

void TestGameProcess() {
    // The game process keeps the paths and options, never the launcher's own choices, and continues this run's log.
    const std::vector<std::string> launcher = {"boot.bin", "--launcher", "--user", "D:\\Games\\DCP\\user", "--log",
                                               "old.log", "--dcp-skip-launcher", "--strict-imports", "--dcp-game"};
    const std::vector<std::string> game = GameProcessArguments(launcher, "D:\\logs\\dragons_crown_pro_042.log");
    const std::vector<std::string> expected = {"boot.bin", "--user", "D:\\Games\\DCP\\user", "--strict-imports",
                                               "--dcp-game", "--log", "D:\\logs\\dragons_crown_pro_042.log"};
    CHECK(game == expected);
    CHECK(std::count(game.begin(), game.end(), std::string(kGameProcessArgument)) == 1);
    CHECK(GameProcessArguments({}, "") == std::vector<std::string>{kGameProcessArgument});
    CHECK(GameProcessArguments(GameProcessArguments({"--app0", "a b"}, "x.log"), "x.log") ==
          GameProcessArguments({"--app0", "a b"}, "x.log"));

    std::string error;
#ifdef _WIN32
    CHECK(WindowsCommandLine(L"C:\\Games\\Dragon Crown\\dragons_crown_pro.exe",
                             {"--user", "C:\\My Games\\user\\", "--log", "a\"b", "", "plain"}) ==
          L"\"C:\\Games\\Dragon Crown\\dragons_crown_pro.exe\" --user \"C:\\My Games\\user\\\\\" --log \"a\\\"b\" \"\" plain");
    // A failed creation reports why and leaves nothing to start.
    CHECK(!CreateGameProcess(fs::temp_directory_path() / "dcp-missing" / "dragons_crown_pro.exe", {kGameProcessArgument},
                             error));
    CHECK(error.find("error 3") != std::string::npos || error.find("error 2") != std::string::npos);
    CHECK(dcp_resume_game_process() == 0);
#else
    CHECK(!CreateGameProcess("dragons_crown_pro", {kGameProcessArgument}, error) && !error.empty());
    CHECK(dcp_resume_game_process() == 0);
#endif

    const fs::path source(DCP_SOURCE_DIR);
    const std::string loader = ReadAll(source / "src" / "loader.c");
    const size_t child = loader.find("if (game_child) {");
    const size_t startup_modules = loader.find("module_diagnostics_report_startup()");
    const size_t launcher_run = loader.find("dcp_launcher_run(");
    const size_t started = loader.find("launched==DCP_LAUNCHER_GAME_STARTED");
    const size_t gpu = loader.find("dcpgpu_init(");
    // The game process lists what it started with and never runs the launcher, so it cannot start another one.
    CHECK(loader.find("module_diagnostics_baseline()") < loader.find("runtime_log_start("));
    CHECK(child != std::string::npos && child < startup_modules && startup_modules < launcher_run);
    CHECK(loader.find("!cpu_only && !check_imports && !game_child") < launcher_run);
    CHECK(loader.find("Overlay compatibility: %s game process") != std::string::npos);
    // This process ends once the game process exists: it never reaches the game's runtime.
    CHECK(started != std::string::npos && started > launcher_run && started < gpu);
    CHECK(loader.find("runtime_log_release()", started) < loader.find("dcp_resume_game_process()", started));
    CHECK(loader.find("exit(0)", started) < gpu);
    dcp_startup_phase("test phase");
    CHECK(std::string(dcp_startup_phase_current()) == "test phase");
    size_t phase_at = 0;
    for (const char* phase : {"launcher handoff", "launcher returned", "runtime initialization", "imports resolved", "patches installed",
                              "guest image mapped", "linked modules initialization", "guest entry"}) {
        const size_t found = loader.find(std::string("dcp_startup_phase(\"") + phase + "\")");
        CHECK(found != std::string::npos && found > phase_at);
        phase_at = found;
    }
    CHECK(launcher_run > loader.find("dcp_startup_phase(\"launcher handoff\")"));
    // Startup logs stay quiet: the phase is only stored for the crash report.
    CHECK(ReadAll(source / "launcher" / "startup_phase.cpp").find("printf") == std::string::npos);
    // Linux keeps the game in the launcher's process.
    CHECK(loader.find("int game_process=0;\n#ifdef _WIN32\n        game_process=1;\n#else") != std::string::npos);
    // Off returns to the loader exactly as before; only On reaches the game process, and a failure keeps the launcher.
    const std::string launcher_source = ReadAll(source / "launcher" / "launcher.cpp");
    const size_t window_off = launcher_source.find("if (!SeparateGameProcess(config_, settings_)) {\n                return Outcome::kPlay;");
    const size_t direct_off = launcher_source.find("if (!SeparateGameProcess(config, settings)) {\n        return DCP_LAUNCHER_PLAY;");
    CHECK(window_off != std::string::npos && window_off < launcher_source.find("StartGameProcess(config_, error)"));
    CHECK(direct_off != std::string::npos && direct_off < launcher_source.find("StartGameProcess(config, error)"));
    CHECK(launcher_source.find("game_process_started_ = true;") > launcher_source.find("StartGameProcess(config_, error)"));
    CHECK(launcher_source.find("play_.Cancel();") != std::string::npos);
    CHECK(launcher_source.find("Could not start the game.") != std::string::npos);
}

void TestLauncherRenderer() {
    const std::string host = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "host.cpp");
    const size_t function = host.find("SDL_Renderer* CreateLauncherRenderer(SDL_Window* window) {");
    const size_t windows = host.find("#ifdef _WIN32", function);
    const size_t other = host.find("#else", windows);
    const size_t end = host.find("#endif", other);
    CHECK(function != std::string::npos && windows != std::string::npos && other != std::string::npos && end != std::string::npos);

    // Windows asks for the software renderer directly: creating Direct3D 11 first would keep issue #45's teardown race.
    const std::string on_windows = host.substr(windows, other - windows);
    CHECK(on_windows.find("return SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);") != std::string::npos);
    CHECK(on_windows.find("nullptr") == std::string::npos);
    CHECK(on_windows.find("direct3d") == std::string::npos);

    // Other platforms keep the accelerated renderer with the software fallback.
    const std::string elsewhere = host.substr(other, end - other);
    const size_t accelerated = elsewhere.find("SDL_CreateRenderer(window, nullptr)");
    CHECK(accelerated != std::string::npos && accelerated < elsewhere.find("SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER)"));

    CHECK(host.find("SDL_CreateRenderer(", end) == std::string::npos);
    CHECK(host.find("renderer_ = CreateLauncherRenderer(window_);") != std::string::npos);
    CHECK(host.find("SDL_GetRendererName(renderer_)") != std::string::npos);
    CHECK(host.find("direct3d11") == std::string::npos && host.find("SDL_HINT_RENDER_DRIVER") == std::string::npos);

    // Teardown order stays textures, ImGui, renderer, window, SDL.
    const size_t textures = host.find("SDL_DestroyTexture(texture);", host.find("void LauncherHost::Close()"));
    const size_t imgui = host.find("ImGui::DestroyContext(imgui_);", textures);
    const size_t renderer = host.find("SDL_DestroyRenderer(renderer_);", imgui);
    const size_t window = host.find("SDL_DestroyWindow(window_);", renderer);
    const size_t quit = host.find("SDL_Quit();", window);
    CHECK(textures != std::string::npos && imgui != std::string::npos && renderer != std::string::npos &&
          window != std::string::npos && quit != std::string::npos);

    // The game's renderer is Vulkan, so the launcher's choice never reaches the GPU shim.
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "window.cpp").find("SDL_SOFTWARE_RENDERER") == std::string::npos);
}

void TestCheatSettings(const fs::path& base) {
    const fs::path file = base / "cheats" / "dcp.ini";
    fs::remove_all(file.parent_path());
    const auto launcher_default = [](std::string_view key) {
        for (const ManagedSetting& setting : ManagedSettings()) {
            if (setting.key == key) {
                return std::string(setting.default_value);
            }
        }
        return std::string("<missing>");
    };
    for (const cheats::CheatInfo& cheat : cheats::kCheats) {
        CHECK(launcher_default(cheat.key) == (cheats::kCheatDefault ? "1" : "0"));
        CHECK(!IsLauncherPlacementSetting(cheat.key));
    }

    // All are off without a file, and after a file that does not mention them.
    CHECK(dcp_saved_cheats(file.string().c_str()) == 0);
    CHECK(dcp_saved_cheats(nullptr) == 0);
    WriteAll(file, "vsync=0\n");
    CHECK(dcp_saved_cheats(file.string().c_str()) == 0);

    // Save keeps them as ordinary settings, one bit each for the loader.
    {
        LauncherSettings settings(file);
        settings.SetBool(cheats::kInfiniteHealthKey, true);
        CHECK(dcp_saved_cheats(file.string().c_str()) == 0); // pending until Save
        CHECK(settings.Save());
    }
    CHECK(dcp_saved_cheats(file.string().c_str()) == cheats::Bit(cheats::Cheat::InfiniteHealth));
    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(cheats::kInfiniteHealthKey, false));
        CHECK(!settings.GetBool(cheats::kInfiniteManaKey, false));
        settings.SetBool(cheats::kInfiniteManaKey, true);
        CHECK(settings.Save());
    }
    CHECK(dcp_saved_cheats(file.string().c_str()) ==
          (cheats::Bit(cheats::Cheat::InfiniteHealth) | cheats::Bit(cheats::Cheat::InfiniteMana)));
    CHECK(WrittenValue(ReadAll(file), cheats::kInfiniteHealthKey, "1"));
    {
        // Every cheat is its own saved setting, and all of them come back from the file.
        LauncherSettings settings(file);
        for (const cheats::CheatInfo& cheat : cheats::kCheats) {
            settings.SetBool(cheat.key, true);
        }
        CHECK(settings.Save());
        CHECK(dcp_saved_cheats(file.string().c_str()) == (1u << cheats::kCheatCount) - 1);
    }
    CHECK(WrittenValue(ReadAll(file), "vsync", "0")); // unrelated lines survive

    // Reset all settings puts them all back to Off, pending until Save.
    {
        LauncherSettings settings(file);
        settings.ResetToDefaults();
        for (const cheats::CheatInfo& cheat : cheats::kCheats) {
            CHECK(!settings.GetBool(cheat.key, true));
        }
        CHECK(settings.Get(cheats::kMovementSpeedKey, "") == "1");
        CHECK(settings.Get(cheats::kLegacyMovementSpeedKey, "") == "0");
        CHECK(settings.Get(cheats::kDamageTakenKey, "") == "100");
        CHECK(settings.Get(cheats::kDamageMultiplierKey, "") == "1");
        CHECK(dcp_saved_cheats(file.string().c_str()) != 0);
        CHECK(settings.Save());
    }
    CHECK(dcp_saved_cheats(file.string().c_str()) == 0);
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);

    // Experience Multiplier: the saved value is the multiplier; a value that is not listed is Off.
    CHECK(launcher_default(cheats::kExperienceMultiplierKey) == "1");
    CHECK(launcher_default(cheats::kDamageTakenKey) == "100");
    WriteAll(file, "cheat_damage_taken=0\n");
    CHECK(dcp_saved_damage_taken(file.string().c_str()) == 0);
    WriteAll(file, "cheat_damage_taken=60\n");
    CHECK(dcp_saved_damage_taken(file.string().c_str()) == 100);
    CHECK(dcp_saved_damage_taken(nullptr) == 100);
    CHECK(launcher_default(cheats::kDamageMultiplierKey) == "1");
    WriteAll(file, "cheat_damage_multiplier=100\n");
    CHECK(dcp_saved_damage_multiplier(file.string().c_str()) == 100);
    WriteAll(file, "cheat_damage_multiplier=7\n");
    CHECK(dcp_saved_damage_multiplier(file.string().c_str()) == 1);
    CHECK(dcp_saved_damage_multiplier(nullptr) == 1);
    CHECK(dcp_saved_experience_multiplier(file.string().c_str()) == 1);
    WriteAll(file, "cheat_experience_multiplier=5\n");
    CHECK(dcp_saved_experience_multiplier(file.string().c_str()) == 5);
    WriteAll(file, "cheat_experience_multiplier=100\n");
    CHECK(dcp_saved_experience_multiplier(file.string().c_str()) == 100);
    WriteAll(file, "cheat_experience_multiplier=7\n");
    CHECK(dcp_saved_experience_multiplier(file.string().c_str()) == 1);
    CHECK(dcp_saved_experience_multiplier(nullptr) == 1);

    // Movement Speed: 1, 2 or 4; a missing or invalid value is Off. The old boolean key gives 2x only when the newer
    // setting is absent, and an explicit value (even an invalid one) wins.
    CHECK(launcher_default(cheats::kMovementSpeedKey) == "1");
    CHECK(launcher_default(cheats::kLegacyMovementSpeedKey) == "0");
    CHECK(cheats::IsListedChoice(cheats::kMovementSpeedChoices, 1));
    CHECK(cheats::IsListedChoice(cheats::kMovementSpeedChoices, 2));
    CHECK(cheats::IsListedChoice(cheats::kMovementSpeedChoices, 4));
    CHECK(!cheats::IsListedChoice(cheats::kMovementSpeedChoices, 3));
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);
    WriteAll(file, "cheat_movement_speed=2\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 2);
    WriteAll(file, "cheat_movement_speed=4\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 4);
    WriteAll(file, "cheat_movement_speed=3\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);
    WriteAll(file, "cheat_movement_speed_2x=1\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 2);
    WriteAll(file, "cheat_movement_speed_2x=0\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);
    WriteAll(file, "cheat_movement_speed_2x=true\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 2);
    WriteAll(file, "cheat_movement_speed=4\ncheat_movement_speed_2x=0\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 4);
    WriteAll(file, "cheat_movement_speed=1\ncheat_movement_speed_2x=1\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);
    WriteAll(file, "cheat_movement_speed=9\ncheat_movement_speed_2x=1\n");
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);
    CHECK(dcp_saved_movement_speed(nullptr) == 1);

    // The game never writes them: the video core's settings file code has no cheat key.
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "dcp_settings.cpp").find("cheat") == std::string::npos);
}

// Online Play wins over the cheats: turning it on writes every cheat Off, the page shows only the lockout line, and
// DCP_ONLINE turns it on as the saved switch does. Nothing else in dcp.ini changes.
void TestOnlineCheatLockout(const fs::path& base) {
    const fs::path file = base / "online_cheats" / "dcp.ini";
    fs::remove_all(file.parent_path());
    WriteAll(file, "online_enabled = 1\ncheat_infinite_health = 1\ncheat_infinite_gold = on\ncheat_movement_speed = 4\n"
                   "cheat_movement_speed_2x = 1\ncheat_experience_multiplier = 100\ncheat_damage_taken = 0\n"
                   "cheat_damage_multiplier = 100\nstory_skip_offer = 1\n"
                   "tutorial_skip_offer = 1\nvsync = 0\n");
    CHECK(dcp_saved_cheats(file.string().c_str()) != 0); // a hand-edited file: the game's lockout ignores this
    {
        LauncherSettings settings(file);
        CHECK(TurnCheatsOff(settings));
        CHECK(!TurnCheatsOff(settings)); // already Off: no further change
        CHECK(settings.Save());
    }
    CHECK(dcp_saved_cheats(file.string().c_str()) == 0);
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == cheats::kMovementSpeedOff);
    CHECK(dcp_saved_experience_multiplier(file.string().c_str()) == cheats::kExperienceMultiplierOff);
    CHECK(dcp_saved_damage_taken(file.string().c_str()) == cheats::kDamageTakenOff);
    CHECK(dcp_saved_damage_multiplier(file.string().c_str()) == cheats::kDamageMultiplierOff);
    CHECK(dcp_skip_offers(file.string().c_str()) == 3);
    CHECK(ReadOnlineConfig(file).enabled);
    CHECK(WrittenValue(ReadAll(file), "vsync", "0"));

    // Turning Online off again leaves the cheats Off and lets the player turn them back on.
    {
        LauncherSettings settings(file);
        settings.SetBool(kOnlineEnabledKey, false);
        settings.SetBool(cheats::kInfiniteHealthKey, true);
        CHECK(settings.Save());
    }
    CHECK(dcp_saved_cheats(file.string().c_str()) == cheats::Bit(cheats::Cheat::InfiniteHealth));

    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    };
    set_env("DCP_ONLINE", "1");
    net::OnlineConfig forced = ReadOnlineConfig(file);
    net::apply_environment(forced);
    CHECK(forced.enabled && ResolveOnlinePlay(false).on);
    set_env("DCP_ONLINE", nullptr);
    CHECK(!ResolveOnlinePlay(false).on);

    CHECK(std::string(cheats::kOnlineLockoutText) == "Cheats are disabled while Online Play is enabled");
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    const size_t page = launcher.find("void PageCheats(trg::Ui& ui) {");
    const size_t locked = launcher.find("if (OnlinePlayOn()) {\n            ui.Help(cheats::kOnlineLockoutText);\n"
                                        "            ui.EndRows();\n            return;", page);
    CHECK(page != std::string::npos && locked != std::string::npos);
    CHECK(locked < launcher.find("ui.Toggle(cheat.label", page));
    CHECK(launcher.find("ResolveOnlinePlay(settings_.GetBool(kOnlineEnabledKey, false)).on") != std::string::npos);
    CHECK(launcher.find("if (OnlinePlayOn() && TurnCheatsOff(settings_))") != std::string::npos);
    CHECK(launcher.find("KeepCheatsOffOnline();") != std::string::npos);
}

bool WrittenValue(const std::string& text, const std::string& key, const std::string& value) {
    return text.find(key + "=" + value) != std::string::npos || text.find(key + " = " + value) != std::string::npos;
}

void TestTextLanguage(const fs::path& base) {
    const fs::path file = base / "language" / "dcp.ini";
    fs::remove_all(file.parent_path());
    const std::string ini = file.string();
    const fs::path us_dir = base / "language" / "us";
    const fs::path asia_dir = base / "language" / "asia";
    WriteAll(us_dir / "image.json", std::string("{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"") + kUs.eboot_sha256 + "\"\n}");
    WriteAll(asia_dir / "image.json", std::string("{\n  \"title_id\": \"CUSA10209\",\n  \"eboot_sha256\": \"") + kCk.eboot_sha256 + "\",\n  \"app_ver\": \"01.02\"\n}");
    const std::string us = us_dir.string();
    const std::string asia = asia_dir.string();

    CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == 1); // no file: English
    CHECK(dcp_text_language(nullptr, nullptr, nullptr) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 10); // no file: Traditional Chinese

    struct Expected { const char* key; const char* name; int id; };
    const Expected western[] = {{"english", "English", 1}, {"french", "French", 2}, {"german", "German", 4},
                                {"italian", "Italian", 5}, {"spanish", "Spanish", 3}};
    // Every build adds Brazilian Portuguese and Russian, the port's translations played in one of its own slots.
    const Expected us_languages[] = {{"english", "English", 1}, {"french", "French", 2}, {"german", "German", 4},
                                     {"italian", "Italian", 5}, {"brazilian_portuguese", "Portuguese (Brazil)", 1},
                                     {"russian", "Russian", 1}, {"spanish", "Spanish", 3}};
    // Japan and Asia/Hong Kong add the port's built-in English, played in the same slot as the other translations.
    const Expected asian[] = {{"traditional_chinese", "Traditional Chinese", 10}, {"korean", "Korean", 9},
                              {"english", "English", 10}, {"brazilian_portuguese", "Portuguese (Brazil)", 10},
                              {"russian", "Russian", 10}};
    const Expected japanese[] = {{"japanese", "Japanese", 0}, {"english", "English", 0},
                                 {"brazilian_portuguese", "Portuguese (Brazil)", 0}, {"russian", "Russian", 0}};
    const auto check_list = [&](const GameBuild* build, std::span<const Expected> expected) {
        const std::span<const TextLanguage> languages = TextLanguagesOf(build);
        CHECK(languages.size() == expected.size());
        for (size_t i = 0; i < expected.size() && i < languages.size(); ++i) {
            CHECK(languages[i].key == expected[i].key && languages[i].name == expected[i].name);
            CHECK(languages[i].ps4_id == expected[i].id);
        }
    };
    check_list(nullptr, western);
    for (const GameBuild& build : SupportedBuilds()) {
        const std::string_view title = build.title_id;
        check_list(&build, title == "CUSA10209"   ? std::span<const Expected>(asian)
                           : title == "CUSA07001" ? std::span<const Expected>(japanese)
                                                  : std::span<const Expected>(us_languages));
    }
    CHECK(TextLanguagesOf(&kUs)[4].translation == "pt-BR" && TextLanguagesOf(&kUs)[4].suffix == "US");
    for (const GameBuild& build : SupportedBuilds()) {
        const bool native_english = std::any_of(build.text_languages.begin(), build.text_languages.end(),
                                                [](const TextLanguage& language) { return language.key == "english"; });
        CHECK(OffersPortEnglish(&build) == !native_english);
        for (const TextLanguage& language : TextLanguagesOf(&build)) {
            const std::string_view expected = language.key == "brazilian_portuguese" ? "pt-BR"
                                              : language.key == "russian"                  ? kPortRussian
                                              : language.key == "english" && !native_english ? kPortEnglish
                                                                                              : "";
            CHECK(language.translation == expected);
        }
        for (const TextLanguage& language : build.text_languages) CHECK(language.translation.empty());
    }

    for (const char* image : {us.c_str(), static_cast<const char*>(nullptr)}) {
        for (const Expected& language : western) {
            LauncherSettings settings(file);
            settings.Set(kTextLanguageKey, language.key);
            CHECK(settings.Save());
            CHECK(WrittenValue(ReadAll(file), "text_language", language.key));
            CHECK(dcp_text_language(ini.c_str(), image, nullptr) == language.id);
        }
    }
    WriteAll(file, "text_language=brazilian_portuguese\n");
    CHECK(IsValidSetting(kTextLanguageKey, "brazilian_portuguese"));
    CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == 1); // the English slot
    CHECK(dcp_text_language(ini.c_str(), nullptr, nullptr) == 1);    // an unknown build: English
    CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 10); // the Traditional Chinese slot
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(ResolveTextLanguage(&build, "brazilian_portuguese").key == "brazilian_portuguese");
        CHECK(ResolveTextLanguage(&build, "russian").key == "russian");
        CHECK(ResolveTextLanguage(&build, "russian").ps4_id == ResolveTextLanguage(&build, "brazilian_portuguese").ps4_id);
    }
    WriteAll(file, "text_language=russian\n");
    CHECK(IsValidSetting(kTextLanguageKey, "russian") && !IsValidSetting(kTextLanguageKey, "Russian"));
    CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 10);
    for (const Expected& language : asian) {
        LauncherSettings settings(file);
        settings.Set(kTextLanguageKey, language.key);
        CHECK(settings.Save());
        CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == language.id);
    }

    // A saved language the active build does not have gives that build's default, and the file keeps it.
    WriteAll(file, "text_language=korean\n");
    CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 9);
    for (const Expected& language : western) {
        WriteAll(file, std::string("text_language=") + language.key + "\n");
        CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 10);
        CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == language.id);
    }
    CHECK(ResolveTextLanguage(&kCk, "english").key == "english" && ResolveTextLanguage(&kCk, "english").ps4_id == 10);
    CHECK(ResolveTextLanguage(&kCk, "korean").key == "korean");
    CHECK(ResolveTextLanguage(&kUs, "korean").key == "english");
    CHECK(ResolveTextLanguage(&kUs, "traditional_chinese").key == "english");
    CHECK(ResolveTextLanguage(&kJp100, "korean").key == "japanese");
    CHECK(ResolveTextLanguage(&kUs, "japanese").key == "english");
    for (const char* absent : {"chinese", "simplified_chinese", "Japanese"}) {
        CHECK(!IsValidSetting(kTextLanguageKey, absent));
        CHECK(ResolveTextLanguage(&kCk, absent).ps4_id == 10);
    }
    // Both CUSA10209 builds share one list: Traditional Chinese and Korean, no Western or Simplified Chinese language.
    for (const GameBuild* asia : {&kCk100, &kCk}) {
        const std::span<const TextLanguage> languages = TextLanguagesOf(asia);
        CHECK(languages.size() == 5 && languages[0].key == "traditional_chinese" && languages[1].key == "korean");
        CHECK(languages[0].ps4_id == 10 && languages[1].ps4_id == 9);
        CHECK(languages[2].key == kEnglishKey && languages[2].ps4_id == 10 && languages[2].suffix == "CT" &&
              languages[2].translation == kPortEnglish);
        CHECK(languages[3].key == kPortugueseBrazilKey && languages[3].ps4_id == 10 && languages[3].suffix == "CT");
        CHECK(languages[4].key == kRussianKey && languages[4].ps4_id == 10 && languages[4].suffix == "CT" &&
              languages[4].translation == kPortRussian);
        for (const char* absent : {"french", "german", "italian", "spanish", "simplified_chinese"}) {
            CHECK(std::none_of(languages.begin(), languages.end(),
                               [&](const TextLanguage& language) { return language.key == absent; }));
        }
    }

    for (const char* unlisted : {"English", "japanese", "fr", "2", ""}) {
        WriteAll(file, std::string("text_language=") + unlisted + "\n");
        CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == 1);
        CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 10);
    }
    WriteAll(file, "show_fps=1\n"); // an existing file without the key
    CHECK(dcp_text_language(ini.c_str(), us.c_str(), nullptr) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str(), nullptr) == 10);
    CHECK(IsValidSetting(kTextLanguageKey, "german") && IsValidSetting(kTextLanguageKey, "korean") &&
          IsValidSetting(kTextLanguageKey, "traditional_chinese") && IsValidSetting(kTextLanguageKey, "japanese"));

    LauncherSettings settings(file);
    settings.ResetToDefaults();
    CHECK(settings.Save());
    CHECK(WrittenValue(ReadAll(file), "text_language", "english"));
}

void TestSettingsSummary(const fs::path& base) {
    const fs::path file = base / "summary" / "dcp.ini";
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "online_enabled = 1\nonline_name = SecretName\nshow_ping = 1\ndisplay_mode = borderless\n"
                           "frame_rate = 120\nshow_fps = 1\ntrue_ultrawide = 1\ndcp_japanese_npc_voices = 1\n"
                           "arachnophobia_safe_mode = 1\nstory_skip_offer = 1\ntutorial_skip_offer = 1\n";
    const LauncherSettings settings(file);
    const DcpRunSettings run = {kUs.label, 1, 2560, 1080, "native", "21:9 Ultrawide", 0, 1u, 0, 0, 1};
    const std::string text = SettingsSummary(settings, &kUs, TextLanguagesOf(&kUs), &run);
    const auto has = [&](const std::string& part) { return text.find(part) != std::string::npos; };
    CHECK(has("Settings: Dragon's Crown Pro PC v" + net::port_version() + ", " + net::host_platform() + ", online protocol " +
              std::to_string(net::kOnlineProtocolVersion) + ", game build " + kUs.label + "\n"));
    CHECK(has("Settings: Online Play on, Ping display on, cheats locked for Online Play\n"));
    CHECK(has("Settings: Window Borderless Fullscreen, frame rate 120 FPS, frame counter on,"));
    CHECK(has("Settings: Render resolution 2560x1080 (native), aspect ratio 21:9 Ultrawide, True Ultrawide off (requested on)\n"));
    CHECK(has("Settings: Menu & Subtitles English, Japanese NPC Voices on, Arachnophobia Safe Mode on\n"));
    CHECK(has("Settings: Skip Intro off, Japanese Menu Buttons off, Story Skip Offer on, Tutorial Skip Offer off, Unlock Gallery on,"));
    for (const char* secret : {"SecretName", "friend", "Friend", "account", "secret", "token", "DCP_DASHBOARD_TOKEN", "lobby"}) {
        CHECK(!has(secret));
    }

    std::ofstream(file) << "display_mode = windowed\n";
    const LauncherSettings plain(file);
    const std::string offline = SettingsSummary(plain, nullptr, TextLanguagesOf(nullptr), nullptr);
    CHECK(offline.find("game build not a supported build\n") != std::string::npos);
    CHECK(offline.find("Settings: Window Windowed, frame rate 60 FPS (Original), frame counter off,") != std::string::npos);
    CHECK(offline.find("True Ultrawide requested off\n") != std::string::npos);
    CHECK(offline.find("Japanese NPC Voices unsupported") != std::string::npos);
    if (!std::getenv("DCP_ONLINE")) CHECK(offline.find("Settings: Online Play off\n") != std::string::npos);
}

void TestRenderResolution(const fs::path& base) {
    const fs::path source(DCP_SOURCE_DIR);
    const fs::path file = base / "render" / "dcp.ini";
    fs::remove_all(file.parent_path());
    CHECK(dcp_render_resolution(file.string().c_str(), nullptr) == 0); // no file: native
    CHECK(dcp_render_resolution(nullptr, nullptr) == 0);

    struct Expected { const char* key; const char* name; int width, height, pro; };
    const Expected expected[] = {{"native", "Native (1080p)", 1920, 1080, 0}, {"480p", "480p", 854, 480, 0},
                                 {"720p", "720p", 1280, 720, 0},              {"1440p", "1440p", 2560, 1440, 1},
                                 {"1800p", "1800p", 3200, 1800, 1},           {"4K", "4K", 3840, 2160, 1}};
    for (int i = 0; i < DCP_RESOLUTION_COUNT; ++i) {
        CHECK(std::string(dcp_resolutions[i].key) == expected[i].key);
        CHECK(std::string(dcp_resolutions[i].name) == expected[i].name);
        CHECK(dcp_resolutions[i].width == expected[i].width && dcp_resolutions[i].height == expected[i].height);
        CHECK(dcp_resolutions[i].pro_memory == expected[i].pro);
        CHECK(ResolutionLabel(dcp_resolutions[i]).find(expected[i].name) == 0);
        {
            LauncherSettings settings(file);
            settings.Set(kRenderResolutionKey, expected[i].key);
            CHECK(settings.Save());
        }
        CHECK(WrittenValue(ReadAll(file), "render_resolution", expected[i].key));
        CHECK(dcp_render_resolution(file.string().c_str(), nullptr) == i);
    }
    CHECK(ResolutionLabel(dcp_resolutions[0]) == "Native (1080p) \xE2\x80\x94 1920 \xC3\x97 1080");
    {
        LauncherSettings settings(file);
        settings.ResetToDefaults();
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), "render_resolution", "native"));
    CHECK(dcp_render_resolution(file.string().c_str(), nullptr) == 0);
    for (const char* unlisted : {"720P", "900p", "8K", ""}) {
        WriteAll(file, std::string("render_resolution=") + unlisted + "\n");
        CHECK(dcp_render_resolution(file.string().c_str(), nullptr) == 0);
    }

    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == "render_resolution" && setting.default_value == "native";
    }
    CHECK(managed);

    size_t count = 0;
    const DcpResolutionSite* sites = kUs.resolution_sites.data();
    count = kUs.resolution_sites.size();
    CHECK(count == 23);
    uint64_t image_size = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t end = sites[i].offset + sites[i].opcode_length + 4;
        image_size = std::max<uint64_t>(image_size, end);
        const bool width_value = sites[i].value == 1920 || sites[i].value == 3840;
        const bool height_value = sites[i].value == 1080 || sites[i].value == 2160;
        CHECK(sites[i].height ? height_value : width_value); // each site holds the right dimension
        for (size_t j = i + 1; j < count; ++j) {
            const uint32_t other_end = sites[j].offset + sites[j].opcode_length + 4;
            CHECK(end <= sites[j].offset || other_end <= sites[i].offset); // no overlap
        }
    }
    const auto make_image = [&] {
        std::vector<unsigned char> image(image_size + 16, 0x90);
        for (size_t i = 0; i < count; ++i) {
            std::memcpy(&image[sites[i].offset], sites[i].opcode, sites[i].opcode_length);
            std::memcpy(&image[sites[i].offset + sites[i].opcode_length], &sites[i].value, 4);
        }
        return image;
    };
    char error[160];
    for (const DcpResolution& resolution : dcp_resolutions) {
        std::vector<unsigned char> image = make_image();
        CHECK(dcp_apply_resolution(image.data(), image.size(), sites, count, &resolution, error, sizeof(error)));
        for (size_t i = 0; i < count; ++i) {
            uint32_t value = 0;
            std::memcpy(&value, &image[sites[i].offset + sites[i].opcode_length], 4);
            CHECK(value == static_cast<uint32_t>(sites[i].height ? resolution.height : resolution.width));
            CHECK(std::memcmp(&image[sites[i].offset], sites[i].opcode, sites[i].opcode_length) == 0);
        }
    }
    {
        std::vector<unsigned char> image = make_image();
        image[sites[7].offset + sites[7].opcode_length] ^= 1; // one site no longer holds the original value
        const std::vector<unsigned char> before = image;
        CHECK(!dcp_apply_resolution(image.data(), image.size(), sites, count, &dcp_resolutions[3], error, sizeof(error)));
        CHECK(image == before); // nothing written
        CHECK(std::string(error).find("unexpected instruction") != std::string::npos);
        CHECK(!dcp_apply_resolution(image.data(), 0x1000, sites, count, &dcp_resolutions[3], error, sizeof(error)));
    }

    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    CHECK(launcher.find("PageGraphics(trg::Ui& ui)") != std::string::npos);
    CHECK(std::string(kRenderResolutionKey) != kWindowWidthKey && std::string(kRenderResolutionKey) != kWindowHeightKey);
    CHECK(ReadAll(source / "launcher" / "resolution.c").find("kWindow") == std::string::npos);
    CHECK(ReadAll(source / "src" / "runtime_dcp.c").find("is_neo_mode(void) { return neo_mode; }") != std::string::npos);
}

void TestAspectRatio(const fs::path& base) {
    const fs::path file = base / "aspect" / "dcp.ini";
    fs::remove_all(file.parent_path());
    CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) == 0 && dcp_aspect_ratio(nullptr, nullptr) == 0);

    struct Expected { const char* key; const char* name; int width_1080; int width_720; };
    const Expected expected[] = {{"16:9", "16:9 Original", 1920, 1280}, {"16:10", "16:10", 1728, 1152},
                                 {"21:9", "21:9 Ultrawide", 2520, 1680}, {"32:9", "32:9 Super Ultrawide", 3840, 2560},
                                 {"4:3", "4:3", 1440, 960}, {"5:4", "5:4", 1352, 904}};
    for (int i = 0; i < DCP_ASPECT_COUNT; ++i) {
        CHECK(std::string(dcp_aspect_ratios[i].key) == expected[i].key);
        CHECK(std::string(dcp_aspect_ratios[i].name) == expected[i].name);
        CHECK(dcp_aspect_index(expected[i].key) == i);
        {
            LauncherSettings settings(file);
            settings.Set(kAspectRatioKey, expected[i].key);
            CHECK(settings.Save());
        }
        CHECK(WrittenValue(ReadAll(file), "aspect_ratio", expected[i].key));
        CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) == i);
        CHECK(IsValidSetting(kAspectRatioKey, expected[i].key));

        const DcpResolution native = dcp_render_target(&dcp_resolutions[0], &dcp_aspect_ratios[i]);
        const DcpResolution p720 = dcp_render_target(&dcp_resolutions[2], &dcp_aspect_ratios[i]);
        CHECK(native.width == expected[i].width_1080 && native.height == 1080);
        CHECK(p720.width == expected[i].width_720 && p720.height == 720);
        CHECK(native.width % 8 == 0 && p720.width % 8 == 0);
    }
    for (const char* invalid : {"", "16:9 Original", "2:1", "21x9", "4:3x", "16:9:"}) {
        WriteAll(file, std::string("aspect_ratio=") + invalid + "\n");
        CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) == 0);
        CHECK(!std::string(invalid).empty() ? !IsValidSetting(kAspectRatioKey, invalid) : true);
    }
    CHECK(dcp_aspect_index(nullptr) == 0);
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == "aspect_ratio" && setting.default_value == "16:9";
    }
    CHECK(managed);

    // 16:9 is the resolution itself, whatever it is.
    for (const DcpResolution& resolution : dcp_resolutions) {
        const DcpResolution same = dcp_render_target(&resolution, &dcp_aspect_ratios[0]);
        CHECK(same.width == resolution.width && same.height == resolution.height && same.pro_memory == resolution.pro_memory);
    }
    // Render resolution sets the height, the ratio the width; a bigger target than 1080p takes the Pro memory path.
    const DcpResolution wide1440 = dcp_render_target(&dcp_resolutions[3], &dcp_aspect_ratios[2]);
    CHECK(wide1440.width == 3360 && wide1440.height == 1440 && wide1440.pro_memory == 1);
    const DcpResolution narrow1440 = dcp_render_target(&dcp_resolutions[3], &dcp_aspect_ratios[4]);
    CHECK(narrow1440.width == 1920 && narrow1440.height == 1440 && narrow1440.pro_memory == 1);
    CHECK(dcp_render_target(&dcp_resolutions[0], &dcp_aspect_ratios[2]).pro_memory == 1);
    CHECK(dcp_render_target(&dcp_resolutions[0], &dcp_aspect_ratios[4]).pro_memory == 0);
    const DcpResolution wide720 = dcp_render_target(&dcp_resolutions[2], &dcp_aspect_ratios[3]);
    CHECK(wide720.width == 2560 && wide720.height == 720 && wide720.pro_memory == 1);
    CHECK(dcp_render_target(&dcp_resolutions[2], &dcp_aspect_ratios[4]).pro_memory == 0);
    for (const DcpResolution& resolution : dcp_resolutions) {
        for (const DcpAspectRatio& aspect : dcp_aspect_ratios) {
            const DcpResolution target = dcp_render_target(&resolution, &aspect);
            CHECK(static_cast<long>(target.width) * target.height <= 3840L * 2160 || &aspect == &dcp_aspect_ratios[0]);
            CHECK(target.height <= resolution.height);
        }
    }
    const DcpResolution capped = dcp_render_target(&dcp_resolutions[5], &dcp_aspect_ratios[3]);
    CHECK(capped.height < 2160 && static_cast<long>(capped.width) * capped.height <= 3840L * 2160);

    // The patch writes the target into the build's sites; a damaged site refuses the whole image.
    for (const GameBuild& build : SupportedBuilds()) {
        const auto sites = build.resolution_sites;
        if (sites.empty()) {
            continue;
        }
        uint64_t size = 0;
        for (const DcpResolutionSite& site : sites) {
            size = std::max<uint64_t>(size, site.offset + site.opcode_length + 4u);
        }
        std::vector<unsigned char> image(size + 16, 0x90);
        for (const DcpResolutionSite& site : sites) {
            std::memcpy(&image[site.offset], site.opcode, site.opcode_length);
            std::memcpy(&image[site.offset + site.opcode_length], &site.value, 4);
        }
        char error[160];
        const DcpResolution wide = dcp_render_target(&dcp_resolutions[0], &dcp_aspect_ratios[2]);
        std::vector<unsigned char> damaged = image;
        damaged[sites[3].offset + sites[3].opcode_length] ^= 1;
        const std::vector<unsigned char> before = damaged;
        CHECK(!dcp_apply_resolution(damaged.data(), damaged.size(), sites.data(), sites.size(), &wide, error, sizeof(error)));
        CHECK(damaged == before);
        CHECK(dcp_apply_resolution(image.data(), image.size(), sites.data(), sites.size(), &wide, error, sizeof(error)));
        for (const DcpResolutionSite& site : sites) {
            uint32_t value = 0;
            std::memcpy(&value, &image[site.offset + site.opcode_length], 4);
            CHECK(value == static_cast<uint32_t>(site.height ? wide.height : wide.width));
        }
    }

    // Window size choices follow the ratio; 16:9 keeps the list the launcher always had.
    const std::vector<WindowSize> classic = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    CHECK(WindowSizesFor(dcp_aspect_ratios[0]) == classic);
    const std::vector<WindowSize> ultrawide = {{1680, 720}, {2100, 900}, {2520, 1080}, {3360, 1440}, {5040, 2160}};
    CHECK(WindowSizesFor(dcp_aspect_ratios[2]) == ultrawide);
    CHECK(WindowSizesFor(dcp_aspect_ratios[4])[2] == WindowSize({1440, 1080}));
    CHECK(WindowSizesFor(dcp_aspect_ratios[5])[2] == WindowSize({1350, 1080}));
    CHECK(WindowSizeOf(1080, dcp_aspect_ratios[1]) == WindowSize({1728, 1080}));
    CHECK(WindowSizeOf(1080, dcp_aspect_ratios[3]) == WindowSize({3840, 1080}));
}

// Frame rate: 60 (Original) unless saved otherwise, one of eight stored values, 0 for Unlimited; the video core reads
// it from the settings file (gpu/shim/dcp_settings.cpp) and the launcher offers every value for every build.
void TestFrameRate(const fs::path& base) {
    const fs::path file = base / "frame_rate" / "dcp.ini";
    fs::remove_all(file.parent_path());
    CHECK(std::string(kFrameRateKey) == "frame_rate");
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kFrameRateKey && setting.default_value == "60";
    }
    CHECK(managed);
    {
        const LauncherSettings missing(file);
        CHECK(missing.Get(kFrameRateKey, "60") == "60");
    }
    for (const char* valid : {"30", "60", "90", "120", "144", "165", "240", "0"}) {
        CHECK(IsValidSetting(kFrameRateKey, valid));
        WriteAll(file, std::string("frame_rate=") + valid + "\n");
        const LauncherSettings stored(file);
        CHECK(stored.Get(kFrameRateKey, "60") == valid);
    }
    for (const char* invalid : {"", "45", "75", "100", "1000", "-1", "unlimited", "1e2", "60.0"}) {
        CHECK(!IsValidSetting(kFrameRateKey, invalid));
        WriteAll(file, std::string("frame_rate=") + invalid + "\n");
        const LauncherSettings stored(file);
        CHECK(stored.Get(kFrameRateKey, "60") == "60");
    }
    {
        LauncherSettings settings(file);
        settings.Set(kFrameRateKey, "144");
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), kFrameRateKey, "144"));
    CHECK(FrameRateLabel(0) == "Unlimited" && FrameRateLabel(60) == "60 FPS (Original)");
    CHECK(FrameRateLabel(30) == "30 FPS" && FrameRateLabel(240) == "240 FPS");
}

// True Ultrawide: off unless saved on, only for shapes wider than 16:9, and it widens the six horizontal extent loads of
// the pass projections by the target's shape over 16:9 while the vertical ones and the game's constants stay.
void TestTrueUltrawide(const fs::path& base) {
    const fs::path file = base / "true_ultrawide" / "dcp.ini";
    fs::remove_all(file.parent_path());
    CHECK(std::string(kTrueUltrawideKey) == "true_ultrawide");
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kTrueUltrawideKey && setting.default_value == "0";
    }
    CHECK(managed);
    CHECK(dcp_true_ultrawide(nullptr) == 0 && dcp_true_ultrawide("") == 0);
    CHECK(dcp_true_ultrawide(file.string().c_str()) == 0);
    {
        LauncherSettings settings(file);
        CHECK(!settings.GetBool(kTrueUltrawideKey, false));
        settings.SetBool(kTrueUltrawideKey, true);
        CHECK(dcp_true_ultrawide(file.string().c_str()) == 0); // pending until Save
        CHECK(settings.Save());
    }
    CHECK(dcp_true_ultrawide(file.string().c_str()) == 1);
    CHECK(WrittenValue(ReadAll(file), kTrueUltrawideKey, "1"));
    {
        LauncherSettings settings(file);
        CHECK(settings.GetBool(kTrueUltrawideKey, false));
        settings.SetBool(kTrueUltrawideKey, false);
        CHECK(settings.Save());
    }
    CHECK(dcp_true_ultrawide(file.string().c_str()) == 0);
    CHECK(WrittenValue(ReadAll(file), kTrueUltrawideKey, "0"));
    struct Saved {
        const char* text;
        int on;
    };
    for (const Saved& saved : {Saved{"true", 1}, Saved{"false", 0}, Saved{"on", 1}, Saved{"", 0}, Saved{"2", 0},
                               Saved{"garbage", 0}}) {
        WriteAll(file, std::string("true_ultrawide=") + saved.text + "\n");
        CHECK(dcp_true_ultrawide(file.string().c_str()) == saved.on);
    }
    WriteAll(file, "aspect_ratio=21:9\n");
    CHECK(dcp_true_ultrawide(file.string().c_str()) == 0);

    // The factor is the render target's width per height over 16:9's, and 1 for every shape not wider than 16:9.
    for (int i = 0; i < DCP_ASPECT_COUNT; ++i) {
        const DcpAspectRatio& aspect = dcp_aspect_ratios[i];
        const bool wider = std::string(aspect.key) == "21:9" || std::string(aspect.key) == "32:9";
        CHECK(dcp_true_ultrawide_applies(&aspect) == (wider ? 1 : 0));
        for (const DcpResolution& resolution : dcp_resolutions) {
            const DcpResolution target = dcp_render_target(&resolution, &aspect);
            const double scale = dcp_true_ultrawide_scale(&aspect, &target);
            CHECK(wider ? scale > 1.3 && scale == target.width * 9.0 / (target.height * 16.0) : scale == 1.0);
        }
    }
    const DcpResolution native21 = dcp_render_target(&dcp_resolutions[0], &dcp_aspect_ratios[2]);
    const DcpResolution native32 = dcp_render_target(&dcp_resolutions[0], &dcp_aspect_ratios[3]);
    CHECK(dcp_true_ultrawide_scale(&dcp_aspect_ratios[2], &native21) == 1.3125);
    CHECK(dcp_true_ultrawide_scale(&dcp_aspect_ratios[3], &native32) == 2.0);

    // The verified builds share the six sites; the others never get the patch.
    const DcpOrthoSite expected_sites[DCP_PASS_ORTHO_SITE_COUNT] = {
        {0x10686, -640.0f}, {0x106bb, -640.0f}, {0x10699, 640.0f}, {0x106d5, 640.0f}, {0x106f7, -406.0f}, {0x10703, 406.0f}};
    for (int i = 0; i < DCP_PASS_ORTHO_SITE_COUNT; ++i) {
        CHECK(dcp_pass_ortho_sites[i].offset == expected_sites[i].offset);
        CHECK(dcp_pass_ortho_sites[i].value == expected_sites[i].value);
    }
    CHECK(kUs.true_ultrawide && kJp100.true_ultrawide && kJp102.true_ultrawide && kJp103.true_ultrawide);
    CHECK(kCk100.true_ultrawide && kCk.true_ultrawide && kUs101.true_ultrawide);
    CHECK(TrueUltrawideAvailable(nullptr) && TrueUltrawideAvailable(&kUs) && TrueUltrawideAvailable(&kCk));
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(!build.true_ultrawide || !build.resolution_sites.empty() || build.features_by_signature);
        for (const DcpResolutionSite& resolution : build.resolution_sites) {
            for (const DcpOrthoSite& site : dcp_pass_ortho_sites) {
                CHECK(site.offset + 8 <= resolution.offset || resolution.offset + resolution.opcode_length + 4u <= site.offset);
            }
        }
    }

    // A pass loop like the game's: twelve rip-relative loads, the X extents at the sites and the Y extents between them.
    constexpr uint64_t kImage = 0x30000;
    constexpr uint64_t kSlots = kImage + 0x1000;
    constexpr uint32_t kConstants = 0x20000;
    const float constants[8] = {-640.0f, 640.0f, -406.0f, 406.0f, -360.0f, 360.0f, -228.0f, 228.0f};
    struct Load {
        uint32_t offset;
        int constant;
    };
    const Load x_loads[DCP_PASS_ORTHO_SITE_COUNT] = {{0x10686, 0}, {0x106bb, 0}, {0x10699, 1},
                                                     {0x106d5, 1}, {0x106f7, 2}, {0x10703, 3}};
    const Load y_loads[] = {{0x106a5, 4}, {0x106b1, 5}, {0x106e1, 4}, {0x106ed, 5}, {0x1070f, 6}, {0x1071b, 7}};
    const auto write_load = [](std::vector<unsigned char>& image, uint32_t offset, uint32_t constant) {
        const unsigned char vmovss[4] = {0xc5, 0xfa, 0x10, 0x05};
        const int32_t displacement = int32_t(int64_t(constant) - (int64_t(offset) + 8));
        std::memcpy(&image[offset], vmovss, 4);
        std::memcpy(&image[offset + 4], &displacement, 4);
    };
    const auto build_image = [&] {
        std::vector<unsigned char> image(kSlots + 0x1000, 0x90);
        std::memcpy(&image[kConstants], constants, sizeof(constants));
        for (const Load& load : x_loads) {
            write_load(image, load.offset, kConstants + 4u * load.constant);
        }
        for (const Load& load : y_loads) {
            write_load(image, load.offset, kConstants + 4u * load.constant);
        }
        return image;
    };
    const auto apply = [&](std::vector<unsigned char>& image, double scale, uint64_t slots = kSlots) {
        char error[200];
        return dcp_apply_true_ultrawide(image.data(), kImage, slots, dcp_pass_ortho_sites, DCP_PASS_ORTHO_SITE_COUNT,
                                        scale, error, sizeof(error));
    };
    const std::vector<unsigned char> original = build_image();
    const auto load_target = [](const std::vector<unsigned char>& image, uint32_t offset) {
        int32_t displacement;
        std::memcpy(&displacement, &image[offset + 4], 4);
        return int64_t(offset) + 8 + displacement;
    };
    const auto slot_value = [](const std::vector<unsigned char>& image, int i) {
        float value;
        std::memcpy(&value, &image[kSlots + 4 * i], 4);
        return value;
    };
    struct Widened {
        double scale;
        float values[DCP_PASS_ORTHO_SITE_COUNT];
    };
    for (const Widened& widened : {Widened{1.3125, {-840.0f, -840.0f, 840.0f, 840.0f, -532.875f, 532.875f}},
                                   Widened{2.0, {-1280.0f, -1280.0f, 1280.0f, 1280.0f, -812.0f, 812.0f}},
                                   Widened{1.0, {-640.0f, -640.0f, 640.0f, 640.0f, -406.0f, 406.0f}}}) {
        std::vector<unsigned char> image = original;
        CHECK(apply(image, widened.scale) == 1);
        for (int i = 0; i < DCP_PASS_ORTHO_SITE_COUNT; ++i) {
            CHECK(load_target(image, x_loads[i].offset) == int64_t(kSlots) + 4 * i);
            CHECK(slot_value(image, i) == widened.values[i]);
        }
        for (const Load& load : y_loads) {
            CHECK(load_target(image, load.offset) == kConstants + 4 * load.constant);
        }
        CHECK(std::memcmp(&image[kConstants], constants, sizeof(constants)) == 0);
        std::vector<unsigned char> rest = image;
        for (const Load& load : x_loads) {
            std::memcpy(&rest[load.offset + 4], &original[load.offset + 4], 4);
        }
        std::memcpy(&rest[kSlots], &original[kSlots], 4 * DCP_PASS_ORTHO_SITE_COUNT);
        CHECK(rest == original);
    }

    // One site that does not match refuses the whole patch and leaves the image as it was.
    const auto refused = [&](const std::function<void(std::vector<unsigned char>&)>& damage, uint64_t slots = kSlots) {
        std::vector<unsigned char> image = original;
        damage(image);
        const std::vector<unsigned char> before = image;
        return apply(image, 1.3125, slots) == 0 && image == before;
    };
    for (const Load& load : x_loads) {
        CHECK(refused([&](std::vector<unsigned char>& image) { image[load.offset + 1] ^= 1; }));
        CHECK(refused([&](std::vector<unsigned char>& image) { write_load(image, load.offset, uint32_t(kImage)); }));
    }
    CHECK(refused([](std::vector<unsigned char>& image) { image[kConstants + 4 * 3] ^= 1; }));
    CHECK(refused([](std::vector<unsigned char>&) {}, kImage - 4));
    CHECK(refused([](std::vector<unsigned char>&) {}, kSlots + (uint64_t(1) << 32)));
}

// Presentation only fits the finished frame into the window: its own key and default, Letterbox unless Stretch is chosen,
// and no change to the render target or the aspect ratio.
void TestRenderer(const fs::path& base) {
    CHECK(std::string(kRendererKey) == "renderer" && std::string(kDefaultRenderer) == "vulkan");
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kRendererKey && setting.default_value == "vulkan";
    }
    CHECK(managed);
    for (const char* value : {"auto", "vulkan", "opengl"}) {
        CHECK(IsValidSetting(kRendererKey, value));
    }
    for (const char* invalid : {"", "OpenGL", "direct3d", "D3D11"}) {
        CHECK(!IsValidSetting(kRendererKey, invalid));
    }
#ifdef _WIN32
    CHECK(IsValidSetting(kRendererKey, "d3d11"));
#else
    CHECK(!IsValidSetting(kRendererKey, "d3d11"));
#endif
    const fs::path file = base / "renderer" / "dcp.ini";
    fs::remove_all(file.parent_path());
    fs::create_directories(file.parent_path());
    CHECK(LauncherSettings(file).Get(kRendererKey, kDefaultRenderer) == "vulkan");
    {
        LauncherSettings settings(file);
        settings.Set(kRendererKey, "opengl");
        CHECK(settings.Save());
    }
    CHECK(LauncherSettings(file).Get(kRendererKey, kDefaultRenderer) == "opengl");
}

void TestPresentation(const fs::path& base) {
    const fs::path file = base / "presentation" / "dcp.ini";
    fs::remove_all(file.parent_path());

    CHECK(std::string(kPresentationKey) == "presentation" && std::string(kDefaultPresentation) == "letterbox");
    CHECK(std::string(kPresentationChoices[0].key) == "letterbox" && std::string(kPresentationChoices[0].name) == "Letterbox");
    CHECK(std::string(kPresentationChoices[1].key) == "stretch" && std::string(kPresentationChoices[1].name) == "Stretch");
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kPresentationKey && setting.default_value == "letterbox";
    }
    CHECK(managed);

    // A missing file or key is Letterbox.
    CHECK(LauncherSettings(file).Get(kPresentationKey, kDefaultPresentation) == "letterbox");
    DcpSettings::Values v;
    CHECK(!DcpSettings::Read(file.string(), v) && v.presentation == DcpGraphics::Letterbox);

    // So is an invalid value.
    for (const char* invalid : {"", "fit", "Stretch", "1"}) {
        WriteAll(file, std::string("presentation=") + invalid + "\n");
        CHECK(!IsValidSetting(kPresentationKey, invalid));
        CHECK(LauncherSettings(file).Get(kPresentationKey, kDefaultPresentation) == "letterbox");
        DcpSettings::Values bad;
        CHECK(DcpSettings::Read(file.string(), bad) && bad.presentation == DcpGraphics::Letterbox);
    }

    CHECK(IsValidSetting(kPresentationKey, "letterbox") && IsValidSetting(kPresentationKey, "stretch"));
    CHECK(DcpGraphics::ParsePresentation("letterbox") == DcpGraphics::Letterbox);
    CHECK(DcpGraphics::ParsePresentation("stretch") == DcpGraphics::Stretch);
    CHECK(DcpGraphics::ParsePresentation("bogus") == DcpGraphics::Letterbox);

    // The choice is saved and read back by both the launcher and the video core.
    for (const char* value : {"letterbox", "stretch"}) {
        fs::remove_all(file.parent_path());
        {
            LauncherSettings settings(file);
            settings.Set(kPresentationKey, value);
            CHECK(settings.Save());
        }
        CHECK(WrittenValue(ReadAll(file), "presentation", value));
        CHECK(LauncherSettings(file).Get(kPresentationKey, kDefaultPresentation) == value);
        DcpSettings::Values saved;
        CHECK(DcpSettings::Read(file.string(), saved) && saved.presentation == DcpGraphics::ParsePresentation(value));
    }

    // An existing file without the key stays valid, and the other settings read as before.
    WriteAll(file, "vsync=0\naspect_ratio=21:9\nrender_resolution=1440p\n");
    CHECK(LauncherSettings(file).Get(kPresentationKey, kDefaultPresentation) == "letterbox");
    DcpSettings::Values old;
    CHECK(DcpSettings::Read(file.string(), old) && !old.vsync && old.presentation == DcpGraphics::Letterbox);
    CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) == 2 && dcp_render_resolution(file.string().c_str(), nullptr) == 3);

    // Changing Presentation leaves the aspect ratio and the render resolution as they are, at every ratio.
    for (const int aspect : {0, 2, 4}) {
        WriteAll(file, std::string("aspect_ratio=") + dcp_aspect_ratios[aspect].key + "\nrender_resolution=1440p\n");
        for (const char* value : {"stretch", "letterbox"}) {
            {
                LauncherSettings settings(file);
                settings.Set(kPresentationKey, value);
                CHECK(settings.Save());
            }
            CHECK(WrittenValue(ReadAll(file), "aspect_ratio", dcp_aspect_ratios[aspect].key));
            CHECK(WrittenValue(ReadAll(file), "render_resolution", "1440p"));
            CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) == aspect);
            CHECK(dcp_render_resolution(file.string().c_str(), nullptr) == 3);
            DcpSettings::Values fit;
            CHECK(DcpSettings::Read(file.string(), fit) && fit.presentation == DcpGraphics::ParsePresentation(value));
        }
    }

    // The launcher's Display page: the label, the choices and the row order (Presentation after VSync).
    const fs::path source(DCP_SOURCE_DIR);
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    const size_t vsync = launcher.find("ui.Toggle(\"VSync\"");
    const size_t presentation = launcher.find("ui.Choice(\"Presentation\", kPresentationHelp, kPresentationKey, kDefaultPresentation");
    const size_t sharpening = launcher.find("ui.Heading(\"Scaling and sharpening\");");
    CHECK(vsync != std::string::npos && presentation != std::string::npos && sharpening != std::string::npos);
    CHECK(vsync < presentation && presentation < sharpening);
    CHECK(launcher.find("\"Controls how the rendered image fits a window or display with a different aspect ratio.\"") !=
          std::string::npos);

    // Letterbox is the aspect-preserving fit; Stretch is the whole swapchain.
    const std::string presenter =
        ReadAll(source / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_presenter.cpp");
    const size_t pick = presenter.find("const vk::ImageBlit blit = stretch");
    const size_t stretch_blit = presenter.find("MakeImageBlitStretch(frame->width", pick);
    const size_t fit_blit = presenter.find("MakeImageBlitFit(frame->width", pick);
    CHECK(pick != std::string::npos && stretch_blit != std::string::npos && fit_blit != std::string::npos);
    CHECK(stretch_blit < fit_blit);
    CHECK(presenter.find("DcpSettings::Get().presentation.load() == DcpGraphics::Stretch") != std::string::npos);
    CHECK(presenter.find("return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);") !=
          std::string::npos);
    CHECK(presenter.find("return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width") != std::string::npos);
}

void TestGraphicsQuality(const fs::path& base) {
    const fs::path source(DCP_SOURCE_DIR);
    const fs::path file = base / "quality" / "dcp.ini";
    fs::remove_all(file.parent_path());
    bool smaa_default = false, aniso_default = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        smaa_default |= setting.key == "smaa" && setting.default_value == "0";
        aniso_default |= setting.key == "anisotropy" && setting.default_value == "0";
    }
    CHECK(smaa_default && aniso_default);
    for (const char* level : {"2", "4", "8", "16"}) {
        {
            LauncherSettings settings(file);
            settings.SetBool(kAntiAliasingKey, true);
            settings.Set(kAnisotropyKey, level);
            CHECK(settings.Save());
        }
        const std::string text = ReadAll(file);
        CHECK(WrittenValue(text, "smaa", "1"));
        CHECK(WrittenValue(text, "anisotropy", level));
        LauncherSettings reread(file);
        CHECK(reread.GetBool(kAntiAliasingKey, false));
        CHECK(reread.Get(kAnisotropyKey, "0") == level);
    }
    {
        LauncherSettings settings(file);
        settings.ResetToDefaults();
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), "smaa", "0"));
    CHECK(WrittenValue(ReadAll(file), "anisotropy", "0"));

    for (const int level : {0, 2, 4, 8, 16}) {
        CHECK(DcpGraphics::ParseAnisotropy(level) == level);
    }
    for (const int level : {1, 3, 32, -2}) {
        CHECK(DcpGraphics::ParseAnisotropy(level) == 0);
    }
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    const size_t resolution = launcher.find("\"Render resolution\"");
    const size_t anti_aliasing = launcher.find("\"Anti-aliasing\"");
    const size_t filtering = launcher.find("\"Texture filtering\"");
    CHECK(resolution != std::string::npos && resolution < anti_aliasing && anti_aliasing < filtering);
    CHECK(launcher.find("MSAA") == std::string::npos && launcher.find("Multisampling") == std::string::npos);
}

void TestGraphicsCard(const fs::path& base) {
    const fs::path source(DCP_SOURCE_DIR);
    const fs::path file = base / "gpu" / "dcp.ini";
    fs::remove_all(file.parent_path());

    const std::vector<std::string> devices = {"NVIDIA GeForce RTX 4070", "", "Intel(R) UHD Graphics 730"};
    const std::vector<std::string> choices = DcpGraphics::GpuChoices(devices);
    CHECK(choices.size() == 2);
    CHECK(choices[0] == "NVIDIA GeForce RTX 4070" && choices[1] == "Intel(R) UHD Graphics 730");
    for (const std::string& choice : choices) {
        CHECK(choice != "automatic" && choice.find("Automatic") == std::string::npos);
    }
    CHECK(DcpGraphics::GpuChoices({}).empty());

    CHECK(DcpGraphics::ResolveGpuByName(devices, "NVIDIA GeForce RTX 4070") == 0);
    CHECK(DcpGraphics::ResolveGpuByName(devices, "Intel(R) UHD Graphics 730") == 2);
    CHECK(DcpGraphics::ResolveGpuByName(devices, "Intel(R) UHD Graphics 7") == 0); // exact names only
    CHECK(DcpGraphics::ResolveGpuByName(devices, "automatic") == 0);                // retired value
    CHECK(DcpGraphics::ResolveGpuByName(devices, "") == 0);                         // no saved card
    CHECK(DcpGraphics::ResolveGpuByName(devices, "AMD Radeon RX 7800 XT") == 0);    // removed card
    CHECK(DcpGraphics::ResolveGpuByName({"", "Intel(R) UHD Graphics 730"}, "") == 1);
    CHECK(DcpGraphics::ResolveGpuByName({"", ""}, "automatic") == -1);

    // The setting has no hardcoded default: the launcher resolves the first listed card, as the renderer does.
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kGraphicsCardKey && setting.default_value.empty();
    }
    CHECK(managed);

    const auto resolve_and_save = [&](const std::string& stored) {
        WriteAll(file, "graphics_card = " + stored + "\n");
        LauncherSettings settings(file);
        const int found = DcpGraphics::ResolveGpuByName(choices, settings.Get(kGraphicsCardKey));
        CHECK(found >= 0);
        settings.Set(kGraphicsCardKey, choices[found]);
        CHECK(settings.Save());
        return ReadAll(file);
    };
    const std::string legacy = resolve_and_save("automatic");
    CHECK(WrittenValue(legacy, kGraphicsCardKey, choices[0]));
    CHECK(legacy.find("automatic") == std::string::npos);
    CHECK(WrittenValue(resolve_and_save(""), kGraphicsCardKey, choices[0]));
    CHECK(WrittenValue(resolve_and_save("NVIDIA GeForce RTX 4070"), kGraphicsCardKey, choices[0]));
    CHECK(WrittenValue(resolve_and_save("Intel(R) UHD Graphics 730"), kGraphicsCardKey, choices[1]));
    CHECK(WrittenValue(resolve_and_save("AMD Radeon RX 7800 XT"), kGraphicsCardKey, choices[0]));

    // The launcher offers the real devices alone; the renderer resolves the same stored name.
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    CHECK(launcher.find("\"Automatic\"") == std::string::npos);
    CHECK(launcher.find("kAutomaticGpu") == std::string::npos);
    CHECK(launcher.find("Automatic prefers a discrete card") == std::string::npos);
    CHECK(launcher.find("Automatic: Xbox controllers") != std::string::npos); // unrelated text stays
    CHECK(launcher.find("DcpGraphics::ResolveGpuByName(gpu_names_") != std::string::npos);
    const size_t monitor = launcher.find("ui.Row(\"Monitor\"");
    const size_t card = launcher.find("ui.Row(\"Graphics card\"");
    const size_t vsync = launcher.find("ui.Toggle(\"VSync\"");
    CHECK(monitor != std::string::npos && monitor < card && card < vsync);

    const std::string instance = ReadAll(source / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_instance.cpp");
    const size_t requested = instance.find("const std::string& requested = DcpSettings::Get().graphics_card;");
    const size_t resolved = instance.find("DcpGraphics::ResolveGpuByName(names, requested)");
    CHECK(requested != std::string::npos && resolved != std::string::npos && requested < resolved);
    CHECK(instance.find("GPU: requested %s is not available") != std::string::npos);
    CHECK(instance.find("GPU: selection Automatic") == std::string::npos);
    CHECK(instance.find("IsDeviceUsable(physical)") != std::string::npos);
}

void TestLauncherWanted() {
    CHECK(LauncherWanted(false, false, false, false));  // not ready: always
    CHECK(LauncherWanted(false, false, false, true));   // even when skipped once
    CHECK(LauncherWanted(true, true, false, false));    // ready, setting on
    CHECK(!LauncherWanted(true, false, false, false));  // ready, setting off
    CHECK(LauncherWanted(true, false, true, false));    // Shift forces it
    CHECK(!LauncherWanted(true, true, true, true));     // skipped once
}

void TestPlayGate() {
    {
        PlayGate gate;
        int saves = 0;
        const auto save = [&] { ++saves; return true; };
        CHECK(gate.Request(false, true, save) == PlayRequest::kStarting);
        CHECK(gate.starting());
        CHECK(gate.Request(false, true, save) == PlayRequest::kBusy); // no double trigger
        CHECK(saves == 1);
    }
    {
        PlayGate gate;
        int saves = 0;
        CHECK(gate.Request(false, false, [&] { ++saves; return true; }) == PlayRequest::kFilesNotReady);
        CHECK(saves == 0 && !gate.starting());
        CHECK(gate.Request(true, true, [&] { ++saves; return true; }) == PlayRequest::kBusy);
        CHECK(saves == 0);
    }
    {
        PlayGate gate;
        CHECK(gate.Request(false, true, [] { return false; }) == PlayRequest::kSaveFailed);
        CHECK(!gate.starting());
    }
    {
        // A game process that could not be created leaves PLAY usable again.
        PlayGate gate;
        int saves = 0;
        const auto save = [&] { ++saves; return true; };
        CHECK(gate.Request(false, true, save) == PlayRequest::kStarting);
        gate.Cancel();
        CHECK(!gate.starting());
        CHECK(gate.Request(false, true, save) == PlayRequest::kStarting && saves == 2);
    }
}

void TestPlacement() {
    const std::vector<Rect> areas = {{0, 0, 1920, 1080}, {1920, 0, 3840, 1080}};
    WindowPlacement on_second{{2000, 100, 3280, 820}};
    WindowPlacement gone{{5000, 100, 6280, 820}};
    WindowPlacement tiny{{10, 10, 100, 100}};
    CHECK(PlacementIsVisible(on_second, areas));
    CHECK(!PlacementIsVisible(gone, areas));
    CHECK(!PlacementIsVisible(tiny, areas));
    CHECK(!PlacementIsVisible(on_second, std::vector<Rect>{{0, 0, 1920, 1080}})); // that display was unplugged

    PlacementTracker tracker;
    const WindowState first{{100, 100, 1380, 820}};
    tracker.Observe(first, false, 0.0);
    CHECK(!tracker.TakeSettled(10.0)); // the starting window is not news
    const WindowState moved{{200, 150, 1480, 870}};
    tracker.Observe(moved, false, 1.0);
    CHECK(!tracker.TakeSettled(1.2)); // still settling
    const auto settled = tracker.TakeSettled(1.6);
    CHECK(settled && settled->normal == moved.rect);
    CHECK(!tracker.TakeSettled(5.0)); // saved once

    WindowState maximized{{0, 0, 1920, 1040}, true, false};
    tracker.Observe(maximized, false, 6.0);
    const auto kept = tracker.TakeSettled(7.0);
    CHECK(kept && kept->maximized && kept->normal == moved.rect); // the restored rectangle is kept
    tracker.Observe(maximized, true, 8.0);
    const auto fullscreen = tracker.TakeSettled(9.0);
    CHECK(fullscreen && fullscreen->fullscreen && fullscreen->normal == moved.rect);
}

void TestOptions() {
    const std::vector<WindowSize> standard = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    CHECK(WindowSizesFor(dcp_aspect_ratios[0]) == standard); // independent of the monitor and of the selection
    for (const DcpAspectRatio& aspect : dcp_aspect_ratios) {
        const std::vector<WindowSize> sizes = WindowSizesFor(aspect);
        CHECK(sizes.size() == std::size(kWindowHeights)); // the entry count never depends on the selection
        for (size_t i = 0; i < sizes.size(); ++i) {
            CHECK(sizes[i].height == kWindowHeights[i] && sizes[i].width > 0);
        }
    }
    CHECK(WindowSizeLabel({1920, 1080}).find("game's own size") != std::string::npos);
    CHECK(WindowSizeLabel({3840, 2160}).find("game's own size") == std::string::npos);

    CHECK(WindowSizeLabel({1280, 720}) == "1280 \xC3\x97 720");
    CHECK(WindowSizeLabel(kGamePictureSize).find("1920 \xC3\x97 1080") == 0);

    std::vector<Display> displays(2);
    displays[0].bounds = {0, 0, 3840, 2160};
    displays[0].primary = true;
    displays[1].bounds = {3840, 0, 1920, 1080};
    CHECK(MonitorLabel(1, displays).find("Display 1 (primary)") == 0);
    CHECK(MonitorLabel(2, displays).find("Display 2") == 0);
    CHECK(MonitorLabel(3, displays).find("not connected") != std::string::npos);
    CHECK(DisplayForSetting(displays, 2) == &displays[1]);
    CHECK(DisplayForSetting(displays, 0) == &displays[0]);
    CHECK(DisplayForSetting(displays, 9) == &displays[0]);
    CHECK(DisplayForSetting({}, 1) == nullptr);

    int count = 0;
    for (const PageId page : kPageOrder) {
        CHECK(std::string(PageName(page)).size() > 0);
        ++count;
    }
    CHECK(count == 10);
    // Online sits between Gameplay and Cheats: it changes play and turns the cheats off.
    const char* const order[] = {"Play",   "Display",  "Graphics", "Gameplay", "Online",
                                 "Cheats", "Controls", "Trophies", "Bug Report", "About"};
    for (size_t i = 0; i < std::size(order); ++i) {
        CHECK(std::string(PageName(kPageOrder[i])) == order[i]);
    }
    int online_entries = 0;
    for (size_t i = 0; i < std::size(kPageOrder); ++i) {
        if (kPageOrder[i] == PageId::kOnline) {
            ++online_entries;
            CHECK(i > 0 && kPageOrder[i - 1] == PageId::kGameplay);
            CHECK(i + 1 < std::size(kPageOrder) && kPageOrder[i + 1] == PageId::kCheats);
        }
    }
    CHECK(online_entries == 1);

    // The sidebar lists the pages in this order (its buttons, and so keyboard and controller navigation, follow the
    // list), starts on the first one, and the friend request badge belongs to the Online page wherever it sits.
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("for (const PageId page : kPageOrder) {\n            c.pages.push_back(MakePage(page));") !=
          std::string::npos);
    CHECK(launcher.find("start_page") == std::string::npos && kPageOrder[0] == PageId::kPlay);
    const size_t online_case = launcher.find("case PageId::kOnline:");
    CHECK(online_case != std::string::npos &&
          launcher.find("online_page_->PendingRequests()", online_case) < launcher.find("case PageId::kTrophies:", online_case));

    // The update banner sits on the same header as the launcher.
    const fs::path source(DCP_SOURCE_DIR);
    CHECK(ReadAll(source / "launcher" / "updater" / "update_ui.cpp").find("trg::layout::HeaderHeight(") != std::string::npos);
    const std::string compact = ReadAll(source / "gpu" / "patches" / "trg-launcher" / "0006-compact-layout.patch");
    CHECK(compact.find("inline float HeaderHeight(") != std::string::npos);
    CHECK(compact.find("ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize") != std::string::npos);
}

// ---- Launcher sidebar and footer layout ----

void TestChromeLayout() {
    using namespace trg::layout;
    const auto same = [](std::string_view a, std::string_view b) { return a == b; };

    CHECK(kTextPage > kTextSection && kTextSection > kTextLabel && kTextLabel > kTextBody && kTextBody > kTextHelp);
    CHECK(kTextSidebar == 17.0f && kTextFooter == 15.0f);
    CHECK(kTextPage == 30.0f && kTextSection == 19.0f && kTextLabel == 17.0f && kTextBody == 16.0f && kTextHelp == 15.0f);
    // Midnight panel < row line < panel line < secondary text, each a cool blue-gray.
    constexpr float kPanelFill[3] = {0.055f, 0.082f, 0.165f}, kSecondaryText[3] = {0.580f, 0.659f, 0.800f};
    for (int c = 0; c < 3; ++c) {
        CHECK(kPanelFill[c] < kRowBorder[c] && kRowBorder[c] < kPanelBorder[c] && kPanelBorder[c] < kSecondaryText[c]);
    }
    CHECK(kPanelBorder[0] < kPanelBorder[1] && kPanelBorder[1] < kPanelBorder[2]);
    CHECK(kRowBorder[0] < kRowBorder[1] && kRowBorder[1] < kRowBorder[2]);
    CHECK(SidebarLabelX() == kSidebarIconInset + kSidebarIcon + kSidebarIconGap);
    constexpr float kMinimumHeight = 560.0f;
    const int pages = int(std::size(dcp::kPageOrder));
    const float shortest_body = kMinimumHeight - HeaderHeight(kMinimumHeight, 1.0f) - kBodyGap - kFooterHeight;
    CHECK(SidebarHeight(pages, SidebarRowFor(pages, shortest_body)) <= shortest_body);

    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float row = kSidebarRow * scale, center = std::floor(row * 0.5f);
        CHECK(LineThickness(scale) >= 1.0f && LineThickness(scale) <= std::ceil(scale));
        CHECK(PassiveLineThickness(scale) >= 1.0f && PassiveLineThickness(scale) <= scale);
        CHECK(PanelBorderWidth(scale) == LineThickness(scale) + 1.0f && PanelBorderWidth(scale) <= 3.0f);
        CHECK(LineThickness(scale) == std::floor(LineThickness(scale)));
        const float icon = kSidebarIcon * scale, text = kTextSidebar * scale;
        const float icon_top = CenteredTop(center, icon * 0.5f), text_top = CenteredTop(center, text * 0.55f);
        CHECK(icon_top >= 0.0f && icon_top + icon <= row);
        CHECK(text_top >= 0.0f && text_top + text <= row);
        CHECK(std::abs(icon_top + icon * 0.5f - center) <= 0.5f && std::abs(text_top + text * 0.55f - center) <= 0.5f);
        CHECK((kSidebarIconInset + kSidebarIcon) * scale < SidebarLabelX() * scale);

        const float footer = kFooterHeight * scale, keycap = kKeycapHeight * scale;
        CHECK(kTextFooter * scale < keycap && keycap <= kFooterButtonHeight * scale && keycap < footer);
        CHECK((kFooterHelpLine + kTextFooter) * scale <= footer && kFooterSeparator * scale <= footer);
        const float footer_center = footer * 0.5f;
        CHECK(std::abs(CenteredTop(footer_center, kTextFooter * scale * 0.55f) + kTextFooter * scale * 0.55f - footer_center) <= 0.5f);
    }

    const std::vector<KeyHint> english = ParseKeyHints("Enter  Play     Alt+F4  Quit     Ctrl+S  Save");
    CHECK(english.size() == 3);
    if (english.size() == 3) {
        CHECK(same(english[0].key, "Enter") && same(english[0].action, "Play"));
        CHECK(same(english[1].key, "Alt+F4") && same(english[1].action, "Quit"));
        CHECK(same(english[2].key, "Ctrl+S") && same(english[2].action, "Save"));
    }
    const std::vector<KeyHint> portuguese = ParseKeyHints("Enter  Jogar     Alt+F4  Sair     Ctrl+S  Salvar");
    CHECK(portuguese.size() == 3 && same(portuguese[2].action, "Salvar"));
    CHECK(ParseKeyHints("Enter  Play        Esc  Quit        Ctrl+S  Save").size() == 3);
    const std::vector<KeyHint> plain = ParseKeyHints("No shortcuts here");
    CHECK(plain.size() == 1 && plain[0].key.empty() && same(plain[0].action, "No shortcuts here"));
    CHECK(ParseKeyHints("").empty() && ParseKeyHints("   ").empty());

    const std::vector<std::string_view> note = SplitSentences("Play applies the changes. Save keeps them without starting.");
    CHECK(note.size() == 2 && same(note[0], "Play applies the changes.") && same(note[1], "Save keeps them without starting."));
    CHECK(SplitSentences("One line").size() == 1 && SplitSentences("").empty());

    const fs::path source(DCP_SOURCE_DIR);
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    const std::string ui_text = ReadAll(source / "launcher" / "localization" / "pt_br_ui_text.cpp");
    CHECK(launcher.find("\"Enter  Play     Alt+F4  Quit     Ctrl+S  Save\"") != std::string::npos);
    CHECK(ui_text.find("Enter  Jogar     Alt+F4  Sair     Ctrl+S  Salvar") != std::string::npos);

    // Every sidebar row and every footer line goes through the same centring, from the active font's own metrics.
    const std::string trg = ReadAll(source / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    CHECK(trg.find("ImGui::PushFont(config_.fonts.semibold, layout::kTextSidebar);") != std::string::npos);
    CHECK(trg.find("CapCenter(config_.fonts.semibold, ImGui::GetFontSize())") != std::string::npos);
    CHECK(trg.find("layout::CenteredTop(center_y, cap_center)") != std::string::npos);
    CHECK(trg.find("layout::CenteredTop(center_y, icon_size * 0.5f)") != std::string::npos);
    CHECK(trg.find("pos.x + layout::SidebarLabelX() * s_") != std::string::npos);
    CHECK(trg.find("layout::ParseKeyHints(T(config_.hints.c_str()))") != std::string::npos);
    CHECK(trg.find("layout::SplitSentences(T(config_.note.c_str()))") != std::string::npos);
    CHECK(trg.find("c[ImGuiCol_TableBorderLight] = t.border_row;") != std::string::npos);
    CHECK(trg.find("Col(config_.theme.border_panel)") != std::string::npos);
    CHECK(trg.find("Col(t.frame_hot, 0.8f)") == std::string::npos);
    CHECK(trg.find("layout::PassiveLineThickness(s_)") != std::string::npos);
    CHECK(trg.find("dl->AddRectFilled(min, ImVec2(min.x + block, min.y + block), Col(kSponsorPink)") != std::string::npos);
    CHECK(trg.find("dl->AddImage(ImTextureRef(sponsor.icon), heart_min") != std::string::npos);
    CHECK(kSupportIconHeart < kSupportIconBlock && kSupportIconBlock <= kControlHeight + kItemGap);
    const std::string online = ReadAll(source / "launcher" / "online_page.cpp");
    CHECK(online.find("ui.RowEmphasized(\"Your friend code\", kFriendCodeHelp);") != std::string::npos);
    CHECK(trg.find("RowLabel(label, help, layout::kTextSection, &theme().accent);") != std::string::npos);
    const std::string nanosvg = ReadAll(source / "launcher" / "third_party" / "nanosvg" / "nanosvg.h");
    CHECK(nanosvg.find("size == (size_t)-1l") != std::string::npos && nanosvg.find("size == -1l") == std::string::npos);
    CHECK(trg.find("Col(t.border_row), layout::PassiveLineThickness(s_)") != std::string::npos);
    CHECK(trg.find("dl->AddRectFilled(outer, outer_end, Col(t.border_panel), 12 * s);") != std::string::npos);
    CHECK(trg.find("dl->AddRect(p, ") == std::string::npos);
    CHECK(trg.find("AddRectFilled(outer, outer_end, Col(config_.theme.border_panel)") != std::string::npos);
    CHECK(trg.find("ImGuiTableFlags_BordersInnerH") == std::string::npos);
    const size_t sidebar_end = trg.find("OutlinePanel(size);"), content_end = trg.find("OutlinePanel(size);", sidebar_end + 1);
    CHECK(sidebar_end < trg.find("BeginChild(\"##sidebar\"") && content_end < trg.find("BeginChild(\"##content\""));
    for (const char* page : {"launcher.cpp", "online_page.cpp", "trophy_page.cpp"}) {
        const std::string text = ReadAll(source / "launcher" / page);
        CHECK(text.find("TableBorder") == std::string::npos && text.find("ImGuiCol_Separator") == std::string::npos);
    }
}

void TestHeaderArt() {
    const fs::path source(DCP_SOURCE_DIR);
    const std::string png = ReadAll(source / "assets" / "launcher" / "header-art.png");
    CHECK(png.size() > 24);
    const auto be32 = [&png](size_t at) {
        return (uint32_t(uint8_t(png[at])) << 24) | (uint32_t(uint8_t(png[at + 1])) << 16) | (uint32_t(uint8_t(png[at + 2])) << 8) |
               uint32_t(uint8_t(png[at + 3]));
    };
    CHECK(be32(16) == 3840 && be32(20) == 432);
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    CHECK(launcher.find("LoadLauncherImage(\"header-art.png\", header_art_image_)") != std::string::npos);
    CHECK(launcher.find("SplitHeaderArt(rect)") != std::string::npos);
    CHECK(ReadAll(source / "CMakeLists.txt").find("/assets/launcher/header-art.png") != std::string::npos);
    CHECK(ReadAll(source / "tools" / "package.ps1").find("'header-art.png'") != std::string::npos);
    CHECK(ReadAll(source / "tools" / "package-linux.sh").find("install -m644 assets/launcher/header-art.png") != std::string::npos);

    CHECK(kHeaderArtScale >= 1.15f && kHeaderArtScale <= 1.25f);
    CHECK(kHeaderArtAlphaGain >= 1.15f && kHeaderArtAlphaGain <= 2.2f);
    CHECK(launcher.find("if (width != header_art_width_ || height != header_art_height_) {") != std::string::npos);
    CHECK(launcher.find("height, width, height, kHeaderArtAlphaGain)") != std::string::npos);
    CHECK(launcher.find("c.max.y + trg::layout::kBodyGap * c.scale") != std::string::npos);
    using namespace trg::layout;
    struct Case {
        float width, height, scale;
    };
    bool single_quad_overflows = false;
    for (const Case& window : {Case{1280, 720, 1.0f}, Case{1689, 840, 1.5f}, Case{1920, 1080, 1.5f}, Case{2880, 1620, 2.25f},
                               Case{3840, 2160, 3.0f}, Case{900, 560, 1.0f}, Case{1350, 840, 1.5f}, Case{2025, 1260, 2.25f}}) {
        const float header = HeaderHeight(window.height, window.scale);
        const float art_bottom = header + kBodyGap * window.scale;
        const HeaderArtRect art = PlaceHeaderArt(0.0f, window.width, art_bottom, window.height, window.scale, kHeaderArtAspect);
        const float art_w = art.max_x - art.min_x, art_h = art.max_y - art.min_y;
        CHECK(art.max_x == window.width && art.min_y <= 0.0f && art.max_y >= art_bottom && art_h >= art_bottom);
        CHECK(-art.min_y <= art_h * kHeaderArtTopCrop);
        const float previous = std::ceil(std::max(header, std::clamp(window.height * 0.2f, 120.0f * window.scale, 190.0f * window.scale)));
        const float previous_ink = window.width - std::round(previous * kHeaderArtAspect) * (1.0f - kHeaderArtInkStart);
        CHECK(std::abs(art_w / art_h - kHeaderArtAspect) < 0.01f);
        CHECK(art_w == std::floor(art_w) && art_h == std::floor(art_h));

        const float ink = art.min_x + art_w * kHeaderArtInkStart;
        const float title = std::clamp(header * 0.34f, 30.0f * window.scale, 44.0f * window.scale);
        CHECK(ink > (24.0f * window.scale + title * 9.5f) && ink < window.width * 0.75f);
        CHECK(ink <= previous_ink && art_h >= previous);

        CHECK(header >= 88.0f * window.scale && header <= 140.0f * window.scale && art_bottom - header == kBodyGap * window.scale);

        float next = art.min_x;
        const std::vector<HeaderArtStrip> strips = SplitHeaderArt(art);
        CHECK(strips.size() > 1 && strips.size() == size_t(std::ceil(art_w / float(kHeaderArtStripWidth))));
        for (const HeaderArtStrip& strip : strips) {
            CHECK(strip.min_x == next && strip.max_x > strip.min_x && strip.max_u <= 1.0f);
            CHECK(strip.max_x - strip.min_x <= float(kHeaderArtStripWidth) && strip.max_x <= window.width);
            next = strip.max_x;
            // SDL_SW_BlitTriangle: texture x times the fixed point (2x) triangle area must fit an int.
            const int64_t area = int64_t((strip.max_x - strip.min_x) * 2.0f) * int64_t(art_bottom * 2.0f);
            CHECK(int64_t(art_w) * area <= INT32_MAX && int64_t(art_h) * area <= INT32_MAX);
        }
        CHECK(next == art.max_x);
        const int64_t whole = int64_t(art_w * 2.0f) * int64_t(art_bottom * 2.0f);
        single_quad_overflows = single_quad_overflows || int64_t(art_w) * whole > INT32_MAX;
    }
    CHECK(single_quad_overflows);  // the strips are what keeps the largest windows drawable
    CHECK(kHeaderArtStripWidth >= 32 && kHeaderArtStripWidth <= 256);

    // One size-matched texture, rebuilt only when its size changes, drawn in clipped strips and never as one quad.
    const size_t painter = launcher.find("trg::HeaderPainter LauncherHeader(");
    const size_t painter_end = launcher.find("void Log(const char* format", painter);
    CHECK(painter != std::string::npos && painter_end != std::string::npos);
    const std::string art_painter = launcher.substr(painter, painter_end - painter);
    const size_t strip_loop = art_painter.find("for (const HeaderArtStrip& strip : SplitHeaderArt(rect))");
    CHECK(strip_loop != std::string::npos && art_painter.find("AddImage(") > strip_loop);
    CHECK(art_painter.find("AddImage(") == art_painter.rfind("AddImage("));
    CHECK(art_painter.find("PushClipRect(c.min, ImVec2(c.max.x, bottom), false)") < strip_loop);
    CHECK(art_painter.find("PopClipRect()") > strip_loop);
    CHECK(art_painter.find("art(static_cast<int>((rect.max_x - rect.min_x) * framebuffer + 0.5f),") != std::string::npos);
    CHECK(art_painter.find("PlaceHeaderArt(c.min.y, c.max.x, bottom, height, c.scale, art_aspect)") != std::string::npos);
    CHECK(art_painter.find("_WIN32") == std::string::npos && art_painter.find("__linux__") == std::string::npos);
    const size_t texture_at = launcher.find("ImTextureID HeaderArtTexture(int width, int height) {");
    const size_t rebuild = launcher.find("if (width != header_art_width_ || height != header_art_height_) {", texture_at);
    CHECK(texture_at != std::string::npos && rebuild != std::string::npos);
    CHECK(launcher.find("host_.DestroyTexture(header_art_);", rebuild) < launcher.find("return header_art_;", rebuild));
    CHECK(launcher.find("host_.DestroyTexture(header_art_);", texture_at) > rebuild);
    CHECK(ReadAll(source / "assets" / "launcher" / "header-art.svg").find("<svg") != std::string::npos);

    // A flat image stays flat, a half covered one averages, and transparent texels add no colour.
    std::vector<uint8_t> image(16 * 8 * 4);
    for (size_t i = 0; i < image.size(); i += 4) {
        const bool stroke = (i / 4) % 2 == 0;
        image[i] = stroke ? 200 : 0;
        image[i + 1] = stroke ? 100 : 0;
        image[i + 2] = stroke ? 50 : 0;
        image[i + 3] = stroke ? 255 : 0;
    }
    const std::vector<uint8_t> half = ScaleArtRgba(image.data(), 16, 8, 8, 4);
    CHECK(half.size() == 8 * 4 * 4);
    if (!half.empty()) {
        CHECK(half[0] == 200 && half[1] == 100 && half[2] == 50 && half[3] >= 127 && half[3] <= 128);
    }
    CHECK(ScaleArtRgba(nullptr, 16, 8, 8, 4).empty() && ScaleArtRgba(image.data(), 16, 8, 0, 4).empty());
    const std::vector<uint8_t> brighter = ScaleArtRgba(image.data(), 16, 8, 8, 4, 1.5f);
    CHECK(brighter.size() == half.size() && brighter[0] == 200 && brighter[3] >= 190 && brighter[3] <= 192);
    CHECK(ScaleArtRgba(image.data(), 16, 8, 16, 8, 3.0f)[3] == 255);
}

// ---- Launcher page width ----

// A frame of a page: the scrollbar is only known once a frame has measured the page.
struct PageWidth {
    float content;
    float control_x;
};

PageWidth PageWidthAt(float body_width, float scrollbar_size, float scale) {
    using namespace trg::layout;
    const float content = StableContentWidth(body_width, scrollbar_size);
    return {content, content - ControlColumnWidth(content, scale)};
}

void TestStablePageWidth() {
    using namespace trg::layout;
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float scrollbar = 10.0f * scale;
        const float body = 879.0f * scale;
        const PageWidth first = PageWidthAt(body, scrollbar, scale);   // no scrollbar yet
        const PageWidth second = PageWidthAt(body, scrollbar, scale);  // scrollbar visible
        CHECK(first.content == second.content && first.control_x == second.control_x);
        CHECK(first.content == body - scrollbar);

        const PageWidth resized = PageWidthAt(body - 100.0f, scrollbar, scale);
        CHECK(resized.content == first.content - 100.0f);
    }

    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    const size_t reserve = trg.find("ImGui::SetNextWindowContentSize(ImVec2(layout::StableContentWidth(body_w, ImGui::GetStyle().ScrollbarSize), 0.0f));");
    CHECK(reserve != std::string::npos && reserve < trg.find("ImGui::BeginChild(\"##page_body\""));
    CHECK(trg.find("AlwaysVerticalScrollbar") == std::string::npos);
    CHECK(trg.find("ImGui::PushID(current_page);") < reserve);
}

void TestSidebarProportions() {
    using namespace trg::layout;
    CHECK(kSidebarRow == 42.0f && kSidebarIcon == 21.0f && kTextSidebar == 17.0f);
    CHECK(SidebarLabelX() == 44.0f);

    // The longest labels at the sidebar's size, measured: English and Portuguese in Segoe UI Semibold, then DejaVu Sans Bold.
    constexpr float kLongestLabels[] = {57.9f, 73.7f, 81.8f, 101.9f};
    constexpr float kMinimumHeight = 560.0f;
    const int pages = int(std::size(dcp::kPageOrder));
    const float shortest_body = kMinimumHeight - HeaderHeight(kMinimumHeight, 1.0f) - kBodyGap - kFooterHeight;
    const float compact_row = SidebarRowFor(pages, shortest_body);
    CHECK(SidebarHeight(pages, compact_row) <= shortest_body);
    // Short windows shrink the rows, never below the icon and label, and tall ones keep the full row.
    CHECK(compact_row >= kSidebarRowMin && compact_row < kSidebarRow && kSidebarRowMin > kSidebarIcon && kSidebarRowMin > kTextSidebar);
    CHECK(SidebarRowFor(pages, SidebarHeight(pages)) == kSidebarRow && SidebarRowFor(pages, 900.0f) == kSidebarRow);
    CHECK(SidebarRowFor(pages, 100.0f) == kSidebarRowMin);
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float narrow = SidebarWidth(900.0f * scale, scale), wide = SidebarWidth(3840.0f * scale, scale);
        CHECK(narrow == 168.0f * scale && wide == 176.0f * scale);
        const float row = narrow - kSidebarPadding * scale * 2.0f;
        CHECK(row > 0.0f && kSidebarPadding * scale * 2.0f + row <= narrow);
        for (const float label : kLongestLabels) {
            CHECK((SidebarLabelX() + label) * scale < row);
        }
        CHECK(kTextSidebar * scale <= kSidebarRow * scale);

        // Never wider than the proportional sidebar, and wider windows give the width to the page.
        const float before = std::clamp(1279.0f * scale * 0.15f, 168.0f * scale, 200.0f * scale);
        const float after = SidebarWidth(1279.0f * scale, scale);
        CHECK(after <= before && before - after > 0.0f);
    }
    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    CHECK(trg.find("layout::SidebarRowFor(int(config_.pages.size()), size.y / s_) * s_;") != std::string::npos);
    CHECK(trg.find("const float sidebar_w = layout::SidebarWidth(w, s_);") != std::string::npos);
    CHECK(trg.find("DrawContent(ImVec2(w - sidebar_w - margin * 3, body_h));") != std::string::npos);
}

void TestRowRhythm() {
    using namespace trg::layout;
    CHECK(kRowPadding == 8.0f && kLabelGap == 3.0f && kInfoGridPadding == 5.0f && kControlHeight == 34.0f);
    CHECK(kItemGap + kSectionGap + kItemGap == 22.0f);
    const float previous_row = kControlHeight + 5.0f * 2.0f;
    for (const float help_lines : {1.0f, 2.0f, 4.0f}) {
        const float content = std::max(kControlHeight, kTextLabel + kLabelGap + kTextHelp * help_lines);
        const float row = content + kRowPadding * 2.0f;
        CHECK(row >= content);
        // The control sits in the middle of the row, whatever the text makes its height.
        CHECK((content - kControlHeight) * 0.5f + kControlHeight * 0.5f == content * 0.5f);
        if (help_lines == 1.0f) {
            const float growth = row / previous_row - 1.0f;
            CHECK(growth >= 0.14f && growth <= 0.2f);
        }
    }

    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    CHECK(trg.find("st.CellPadding = ImVec2(0, layout::kRowPadding * s);") != std::string::npos);
    CHECK(trg.find("ImGuiStyleVar_CellPadding, ImVec2(0, layout::kInfoGridPadding * scale_)") != std::string::npos);
    const size_t heading = trg.find("void Ui::Heading("), section = trg.find("bool Ui::Section(");
    CHECK(trg.find("layout::kSectionGap * scale_", heading) < trg.find("ImGui::TextUnformatted(T(text));", heading));
    CHECK(trg.find("layout::kSectionGap * scale_", section) < trg.find("ImGui::CollapsingHeader", section));
    CHECK(trg.find("const float spare = label_h - ImGui::GetFrameHeight();") != std::string::npos);
}

void TestRowSeparators() {
    using namespace trg::layout;
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float row_left = 238.0f * scale, body = 1010.0f * scale, scrollbar = 10.0f * scale;
        const float content = StableContentWidth(body, scrollbar);
        const float separator_right = row_left + content;
        const float control_x = separator_right - ControlColumnWidth(content, scale);
        CHECK(separator_right > control_x && control_x > row_left);
        CHECK(separator_right == row_left + body - scrollbar);
    }

    // Filled after the table, from the width the rows use, on every shared row and the info grid.
    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    CHECK(trg.find("row_right_ = row_left_ + ImGui::GetContentRegionAvail().x;") != std::string::npos);
    CHECK(trg.find("dl->AddRectFilled(ImVec2(row_left_, top), ImVec2(row_right_, top + 1.0f), color);") != std::string::npos);
    const size_t begin_lines = trg.find("    BeginRowLines();\n"), begin_lines_crlf = trg.find("    BeginRowLines();\r\n");
    CHECK(begin_lines != std::string::npos || begin_lines_crlf != std::string::npos);
    const size_t end_rows = trg.find("void Ui::EndRows()");
    CHECK(end_rows != std::string::npos && trg.find("ImGui::EndTable();", end_rows) < trg.find("DrawRowLines();", end_rows));
    const size_t grid = trg.find("void Ui::InfoGrid(");
    CHECK(grid != std::string::npos && trg.find("BeginRowLines();", grid) < trg.find("ImGui::BeginTable(id, 4", grid));
    CHECK(trg.find("ImDrawListFlags_AntiAliasedLines") == std::string::npos);
    for (const char* page : {"launcher.cpp", "online_page.cpp", "trophy_page.cpp", "incident_page.cpp"}) {
        CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / page).find("BordersInnerH") == std::string::npos);
    }
}

// ---- Launcher geometry at the supported display scales ----

struct LogicalWindow {
    float width, height;
};

constexpr LogicalWindow kLauncherWindows[] = {{900, 560}, {1126, 560}, {1280, 720}, {1920, 1080}, {3840, 2160}};
constexpr float kDisplayScales[] = {1.0f, 1.5f, 2.25f};
constexpr float kTolerance = 0.01f;

// Launcher::Frame's layout of a window, in physical pixels.
struct ChromeGeometry {
    float header, body_h, sidebar_w, content_w, page_w, scrollbar, stable, control, label, footer_w;
};

ChromeGeometry ChromeAt(const LogicalWindow& window, float scale) {
    using namespace trg::layout;
    ChromeGeometry g{};
    const float w = window.width * scale, h = window.height * scale, margin = kMargin * scale;
    g.header = HeaderHeight(h, scale);
    g.body_h = std::max(100.0f * scale, h - g.header - kBodyGap * scale - kFooterHeight * scale);
    g.sidebar_w = SidebarWidth(w, scale);
    g.content_w = w - g.sidebar_w - margin * 3.0f;
    g.page_w = g.content_w - kContentPadding * scale * 2.0f;
    g.scrollbar = 10.0f * scale;
    g.stable = StableContentWidth(g.page_w, g.scrollbar);
    g.control = ControlColumnWidth(g.stable, scale);
    g.label = g.stable - g.control;
    g.footer_w = w - margin * 2.0f;
    return g;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t begin = text.find(from);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find(to, begin + from.size());
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void TestLauncherDpiLayout() {
    using namespace trg::layout;
    for (const float scale : kDisplayScales) {
        for (const LogicalWindow& window : kLauncherWindows) {
            const ChromeGeometry g = ChromeAt(window, scale);
            const int pages = int(std::size(dcp::kPageOrder));
            CHECK(SidebarHeight(pages, SidebarRowFor(pages, g.body_h / scale)) * scale <= g.body_h + kTolerance);
            CHECK(g.sidebar_w >= 168.0f * scale && g.sidebar_w <= 176.0f * scale);
            CHECK(g.content_w > 0.0f && std::abs(g.sidebar_w + kMargin * scale * 3.0f + g.content_w - window.width * scale) < kTolerance);

            CHECK(g.stable > 0.0f && g.stable <= g.page_w);
            CHECK(g.control > 0.0f && g.control < g.stable && g.label >= 200.0f * scale);
            CHECK(g.scrollbar > 0.0f && g.scrollbar < g.page_w * 0.05f);

            // Separators run from the row's left edge to the gutter reserved for the scrollbar, inside the right padding.
            const float row_left = kContentPadding * scale, separator_right = row_left + g.stable;
            CHECK(separator_right > row_left + g.label && separator_right <= row_left + g.page_w - g.scrollbar + kTolerance);
            CHECK(separator_right < g.content_w - kContentPadding * scale);

            const float cluster = (kPlayWidth + kFooterButtonWidth * 2.0f + kFooterGap * 2.0f) * scale;
            CHECK(cluster <= g.footer_w && g.footer_w - cluster >= 300.0f * scale);
        }
    }

    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    CHECK(trg.find("HorizontalScrollbar") == std::string::npos);
    CHECK(trg.find("st.ScrollbarSize = 10 * s;") != std::string::npos);
    const std::string sidebar = Between(trg, "void Launcher::DrawSidebar(", "void Launcher::DrawContent(");
    CHECK(sidebar.find("ImGuiWindowFlags_NoScrollbar") != std::string::npos);
    CHECK(sidebar.find("const ImVec2 item(size.x - pad * 2, item_h);") != std::string::npos);
    CHECK(sidebar.find("ImGui::SetCursorPosX(pad);") != std::string::npos);

    // The pages with the densest rows stay on the shared row system, whose control column is fixed.
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    for (const char* page : {"PagePlay", "PageDisplay", "PageGameplay", "PageAbout"}) {
        CHECK(launcher.find(std::string("void ") + page + "(trg::Ui& ui) {") != std::string::npos);
    }
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "online_page.cpp").find("void OnlinePage::Draw(trg::Ui& ui) {") != std::string::npos);
}

void TestSidebarLabels() {
    using namespace trg::layout;
    const fs::path source(DCP_SOURCE_DIR);
    const std::string portuguese = ReadAll(source / "launcher" / "localization" / "pt_br_ui_text.cpp");
    std::string longest_portuguese;
    size_t longest_english = 0;
    for (const PageId page : kPageOrder) {
        const std::string english = PageName(page);
        const size_t row = portuguese.find("\n" + english + "\t");
        CHECK(row != std::string::npos);
        const size_t begin = row + english.size() + 2;
        const std::string label = portuguese.substr(begin, portuguese.find('\n', begin) - begin);
        CHECK(!label.empty() && label.size() <= 12 && english.size() <= 12);
        longest_english = std::max(longest_english, english.size());
        if (label.size() > longest_portuguese.size()) {
            longest_portuguese = label;
        }
    }
    // The measured label widths in TestSidebarProportions end at this label.
    CHECK(longest_portuguese == "Jogabilidade" && longest_english <= longest_portuguese.size());

    for (const float scale : kDisplayScales) {
        const float row = SidebarWidth(900.0f * scale, scale) - kSidebarPadding * scale * 2.0f;
        CHECK(SidebarLabelX() * scale < row && kSidebarIcon * scale <= kSidebarRow * scale);
        for (const LogicalWindow& window : kLauncherWindows) {
            const ChromeGeometry g = ChromeAt(window, scale);
            CHECK(SidebarLabelX() * scale < g.sidebar_w - kSidebarPadding * scale * 2.0f);
            // The sidebar's cap hands every pixel it does not use to the page.
            const float proportional = std::clamp(window.width * scale * 0.15f, 168.0f * scale, 200.0f * scale);
            CHECK(g.sidebar_w <= proportional + kTolerance);
        }
    }
    CHECK(SidebarWidth(3840.0f * 2.25f, 2.25f) < std::clamp(3840.0f * 2.25f * 0.15f, 168.0f * 2.25f, 200.0f * 2.25f));
}

void TestFooterAndCards() {
    using namespace trg::layout;
    const fs::path source(DCP_SOURCE_DIR);
    const std::string trg = ReadAll(source / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    const std::string footer = Between(trg, "void Launcher::DrawFooter(", "void Launcher::DrawPlayButton(");
    CHECK(!footer.empty());
    CHECK(footer.find("const float quit_x = right - play_w - bw * 2 - gap * 2;") != std::string::npos);
    CHECK(footer.find("ImGui::Button(T(\"Quit\"), ImVec2(bw, bh))") != std::string::npos);
    CHECK(footer.find("ImGui::Button(T(\"Save\"), ImVec2(bw, bh))") != std::string::npos);
    CHECK(footer.find("DrawPlayButton(ImVec2(play_w, play_h), check.ok);") != std::string::npos);
    CHECK(footer.find("SponsorButton(\"##sponsor_footer\"") != std::string::npos);
    CHECK(footer.find("if (x >= help_x + note_w + gap) {") != std::string::npos);
    CHECK(footer.find("DrawKeyHints(dl, hints, fonts, t,") != std::string::npos);

    struct Shortcut {
        const char* key;
        const char* action;
    };
    const auto shortcuts = [](const char* text, const std::array<Shortcut, 3>& expected) {
        const std::vector<KeyHint> hints = ParseKeyHints(text);
        CHECK(hints.size() == expected.size());
        for (size_t i = 0; i < hints.size() && i < expected.size(); ++i) {
            CHECK(hints[i].key == expected[i].key && hints[i].action == expected[i].action);
        }
    };
    shortcuts("Enter  Play     Alt+F4  Quit     Ctrl+S  Save", {Shortcut{"Enter", "Play"}, {"Alt+F4", "Quit"}, {"Ctrl+S", "Save"}});
    shortcuts("Enter  Jogar     Alt+F4  Sair     Ctrl+S  Salvar", {Shortcut{"Enter", "Jogar"}, {"Alt+F4", "Sair"}, {"Ctrl+S", "Salvar"}});

    for (const float scale : kDisplayScales) {
        const float footer_h = kFooterHeight * scale;
        const float glow = 7.0f * scale;  // the Play button's pulse reaches this far past its edge
        CHECK(kPlayHeight * scale + glow * 2.0f <= footer_h && kFooterButtonHeight * scale <= footer_h);
        CHECK(kKeycapHeight * scale < footer_h && kSupportIconBlock * scale < footer_h);
        const float width = ChromeAt(kLauncherWindows[0], scale).footer_w;
        CHECK((kPlayWidth + kFooterButtonWidth * 2.0f + kFooterGap * 2.0f) * scale < width);
    }

    // The About page's support card and the footer's Sponsor button share one link.
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    CHECK(std::string(kSponsorUrl) == "https://github.com/sponsors/saulob");
    CHECK(launcher.find("c.sponsor = {\"Sponsor on GitHub\", kSponsorUrl, sponsor_icon_};") != std::string::npos);
    const std::string about = Between(launcher, "void PageAbout(trg::Ui& ui) {", "ui.Paragraph(");
    CHECK(about.find("ui.SponsorCard(") != std::string::npos);
    const std::string card = Between(trg, "void Ui::SponsorCard(", "\nvoid Ui::");
    CHECK(card.find("kSponsorPink") != std::string::npos && card.find("AddImage(ImTextureRef(sponsor.icon)") != std::string::npos);
    CHECK(card.find("if (sponsor.url.empty()) return;") != std::string::npos);
    for (const LogicalWindow& window : kLauncherWindows) {
        for (const float scale : kDisplayScales) {
            const ChromeGeometry g = ChromeAt(window, scale);
            CHECK((kSupportIconBlock + kSupportIconGap) * scale < g.stable * 0.25f);
        }
    }

    // The friend code row: the code, its Copy button, and nothing of the Connection test or the relay.
    const std::string online = ReadAll(source / "launcher" / "online_page.cpp");
    const std::string code = Between(online, "void OnlinePage::DrawFriendCode(", "bool OnlinePage::DrawRequiredTestRow(");
    CHECK(code.find("ui.RowEmphasized(\"Your friend code\", kFriendCodeHelp);") != std::string::npos);
    CHECK(code.find("ImGui::Button(Tr(\"Copy\"), ImVec2(-FLT_MIN, 0))") != std::string::npos);
    CHECK(code.find("ImGui::SetClipboardText(state.friend_code.c_str());") != std::string::npos);
    CHECK(code.find("ImGui::SameLine();") < code.find("ImGui::Button(Tr(\"Copy\")"));
    CHECK(code.find("Connection") == std::string::npos && code.find("DrawConnectionTest") == std::string::npos);
    CHECK(code.find("relay") == std::string::npos && code.find("Relay") == std::string::npos);
    const size_t friend_case = online.find("case OnlineSection::kFriendCode: DrawFriendCode(ui); break;");
    const size_t test_case = online.find("case OnlineSection::kConnectionTest: DrawConnectionTest(ui); break;");
    CHECK(friend_case != std::string::npos && test_case != std::string::npos && friend_case != test_case);

    const std::string portuguese = ReadAll(source / "launcher" / "localization" / "pt_br_ui_text.cpp");
    for (const char* text : {"\nYour friend code\t", "\nCopy\t", "\nConnection test\t", "\nRun test\t", "\nAbout\t"}) {
        CHECK(portuguese.find(text) != std::string::npos);
    }
}

// ---- Launcher scrollbar gutter ----

// What a page lays out from, in the frame before its scrollbar shows and in the one after.
struct PageGeometry {
    float content, control_x, wrap, separator_right;
};

PageGeometry PageGeometryAt(float body_width, float scrollbar, float scale, float row_left) {
    using namespace trg::layout;
    const float content = StableContentWidth(body_width, scrollbar);
    const float control = ControlColumnWidth(content, scale);
    return {content, content - control, content - control - 16.0f * scale, row_left + content};
}

void TestScrollbarSettling() {
    using namespace trg::layout;
    struct Page {
        const char* name;
        LogicalWindow window;
    };
    for (const float scale : kDisplayScales) {
        for (const Page& page : {Page{"Gameplay", {1280, 720}}, Page{"Display", {1920, 1080}}, Page{"Display", {900, 560}},
                                 Page{"Online", {1126, 560}}}) {
            const ChromeGeometry g = ChromeAt(page.window, scale);
            const float row_left = kContentPadding * scale;
            const PageGeometry first = PageGeometryAt(g.page_w, g.scrollbar, scale, row_left);
            const PageGeometry second = PageGeometryAt(g.page_w, g.scrollbar, scale, row_left);
            CHECK(first.content == second.content && first.control_x == second.control_x);
            CHECK(first.wrap == second.wrap && first.separator_right == second.separator_right);
            CHECK(first.wrap > 0.0f && first.content == g.stable && first.control_x == g.label);
        }

        // A resize moves the page by the resize and by nothing else, whether or not a scrollbar is showing.
        const ChromeGeometry small = ChromeAt(kLauncherWindows[0], scale), large = ChromeAt(kLauncherWindows[3], scale);
        const PageGeometry before = PageGeometryAt(small.page_w, small.scrollbar, scale, 0.0f);
        const PageGeometry after = PageGeometryAt(large.page_w, large.scrollbar, scale, 0.0f);
        CHECK(after.content - before.content == large.page_w - small.page_w);
        CHECK(after.content > before.content && after.control_x >= before.control_x);
        CHECK(after.separator_right - before.separator_right == after.content - before.content);
    }

    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    CHECK(trg.find("ScrollbarSizes") == std::string::npos && trg.find("ScrollbarY") == std::string::npos);
    CHECK(trg.find("GetScrollMaxY") == std::string::npos && trg.find("scrollbar_visible") == std::string::npos);
}

// ---- Launcher separators and row spacing ----

void TestSeparatorPages() {
    using namespace trg::layout;
    struct Page {
        const char* name;
        LogicalWindow window;
    };
    for (const float scale : kDisplayScales) {
        for (const Page& page : {Page{"Display", {1280, 720}}, Page{"Gameplay", {1920, 1080}}, Page{"Graphics", {900, 560}},
                                 Page{"Online", {1126, 560}}}) {
            const ChromeGeometry g = ChromeAt(page.window, scale);
            const float row_left = kContentPadding * scale, right = row_left + g.stable;
            const float control_x = right - g.control, text_end = row_left + g.label - 16.0f * scale;
            CHECK(row_left < text_end && text_end < control_x && control_x < right);
            CHECK(right - row_left == g.stable && right + g.scrollbar == row_left + g.page_w);
        }
    }

    // The lines come from the width the rows use, not from a cell, so a clipped column cannot shorten them.
    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    const std::string row = Between(trg, "void Ui::BeginRow() {", "void Ui::RowLabel(");
    CHECK(row.find("BeginRowLines();") < row.find("ImGui::BeginTable(id, 2"));
    const std::string lines = Between(trg, "void Ui::DrawRowLines() {", "\nint Ui::Segmented(");
    CHECK(lines.find("row_left_") != std::string::npos && lines.find("row_right_") != std::string::npos);
    CHECK(lines.find("TableGetColumn") == std::string::npos && lines.find("GetColumnWidth") == std::string::npos);
    CHECK(lines.find("Col(theme().border_row)") != std::string::npos);

    // Border colours come from the theme's two roles; no page picks its own.
    const fs::path launcher_dir = fs::path(DCP_SOURCE_DIR) / "launcher";
    for (const char* file : {"launcher.cpp", "trophy_page.cpp"}) {
        const std::string text = ReadAll(launcher_dir / file);
        CHECK(text.find("border_row") == std::string::npos && text.find("border_panel") == std::string::npos);
        CHECK(text.find("kRowBorder") == std::string::npos && text.find("kPanelBorder") == std::string::npos);
    }
    // The Online page draws two lines the rows do not: under its tabs and around the lobby list, in those same roles.
    const std::string online = ReadAll(launcher_dir / "online_page.cpp");
    const auto occurrences = [](const std::string& text, std::string_view word) {
        size_t count = 0;
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) ++count;
        return count;
    };
    CHECK(online.find("kRowBorder") == std::string::npos && online.find("kPanelBorder") == std::string::npos);
    CHECK(occurrences(online, "border_row") == 1 && occurrences(online, "border_panel") == 1);
    CHECK(Between(online, "void OnlinePage::DrawTabs(", "void OnlinePage::DrawFriendRequests(").find("t.border_row") != std::string::npos);
    CHECK(Between(online, "void OnlinePage::DrawLobbyList(", "void OnlinePage::DrawLobbyPlayers(").find("t.border_panel") != std::string::npos);
    for (const char* file : {"launcher.cpp", "trophy_page.cpp"}) {
        CHECK(ReadAll(launcher_dir / file).find("ImGuiCol_Border") == std::string::npos);
    }
    const std::string theme = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "include" / "trg" / "launcher.h");
    CHECK(theme.find("ImVec4 border_row = ") != std::string::npos && theme.find("ImVec4 border_panel = ") != std::string::npos);
}

void TestRowKinds() {
    using namespace trg::layout;
    // A row is as tall as its text or its control, whichever is taller, plus the padding above and below.
    const auto height = [](float help_lines, float control) {
        const float text = help_lines > 0.0f ? kTextLabel + kLabelGap + kTextHelp * help_lines : kTextLabel;
        return std::max(control, text) + kRowPadding * 2.0f;
    };
    const float short_row = height(0.0f, kControlHeight), one_line = height(1.0f, kControlHeight);
    const float wrapped = height(3.0f, kControlHeight), long_wrapped = height(5.0f, kControlHeight);
    CHECK(short_row == kControlHeight + kRowPadding * 2.0f && one_line >= short_row);
    CHECK(wrapped > one_line && long_wrapped - wrapped == kTextHelp * 2.0f);
    for (const float help_lines : {0.0f, 1.0f, 3.0f, 5.0f}) {
        const float row = height(help_lines, kControlHeight);
        const float content = row - kRowPadding * 2.0f, spare = content - kControlHeight;
        CHECK(spare >= 0.0f && spare * 0.5f + kControlHeight + spare * 0.5f == content);
    }

    // Combo, segmented, slider and the other controls all start from the shared row, which centres them.
    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    for (const char* control : {"bool Ui::Combo(", "bool Ui::Choice(", "bool Ui::SliderInt(", "bool Ui::SliderFloat(", "bool Ui::Toggle("}) {
        const std::string body = Between(trg, control, "\nbool Ui::");
        CHECK(!body.empty() && body.find("Row(label, help);") != std::string::npos);
    }
    CHECK(Between(trg, "bool Ui::Choice(", "\nbool Ui::").find("Segmented(key, labels, sel)") != std::string::npos);
    CHECK(Between(trg, "void Ui::RowControl() {", "\nvoid Ui::EndRows()").find("spare * 0.5f") != std::string::npos);
    CHECK(Between(trg, "void Ui::RowLabel(", "\nvoid Ui::Row(").find("PushTextWrapPos") != std::string::npos);
}

// ---- Launcher shared source ----

void TestSharedLauncherSource() {
    const fs::path source(DCP_SOURCE_DIR);
    const std::string windows_only = "windows.h|winhttp|_WIN32|HINTERNET|<io.h>|<direct.h>|WINAPI";
    std::vector<fs::path> shared = {source / "launcher" / "header_art.h", source / "launcher" / "header_art.cpp",
                                    source / "launcher" / "page_icons.h", source / "launcher" / "page_icons.cpp",
                                    source / "third_party" / "trg-launcher" / "include" / "trg" / "layout.h",
                                    source / "third_party" / "trg-launcher" / "include" / "trg" / "page_navigation.h"};
    for (const fs::path& file : shared) {
        const std::string text = ReadAll(file);
        CHECK(!text.empty());
        for (size_t begin = 0; begin < windows_only.size();) {
            const size_t end = windows_only.find('|', begin);
            const std::string needle = windows_only.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            CHECK(text.find(needle) == std::string::npos);
            begin = end == std::string::npos ? windows_only.size() : end + 1;
        }
    }
    // The same launcher code draws on both platforms; only the renderer SDL picks differs.
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    const std::string painter = Between(launcher, "trg::HeaderPainter LauncherHeader(", "void Log(const char* format");
    CHECK(!painter.empty() && painter.find("#if") == std::string::npos);
    const std::string build = ReadAll(source / "launcher" / "CMakeLists.txt");
    CHECK(build.find("header_art.cpp") != std::string::npos && build.find("page_icons.cpp") != std::string::npos);
    CHECK(build.find("if (WIN32)") != std::string::npos && build.find("OpenSSL::Crypto") != std::string::npos);
    CHECK(ReadAll(source / "tools" / "package-linux.sh").find("install -m644 assets/launcher/header-art.png") != std::string::npos);
    CHECK(ReadAll(source / "tools" / "package-linux.sh").find("usr/bin/assets/launcher/header-art.png") != std::string::npos);
}

// ---- Launcher page icons ----

void TestPageIcons() {
    const fs::path source(DCP_SOURCE_DIR);
    const fs::path icons = source / "assets" / "launcher" / "icons";

    // The sidebar's pages and their icons, one to one.
    const std::set<std::string> expected = {"play.svg",   "display.svg",  "graphics.svg", "gameplay.svg",   "online.svg",
                                            "cheats.svg", "controls.svg", "trophies.svg", "bug-report.svg", "about.svg"};
    std::set<std::string> mapped_files;
    std::set<int> mapped_pages, ordered_pages;
    for (const PageIconAsset& asset : PageIconAssets()) {
        mapped_files.insert(asset.file);
        mapped_pages.insert(static_cast<int>(asset.page));
    }
    for (const PageId page : kPageOrder) {
        ordered_pages.insert(static_cast<int>(page));
    }
    CHECK(expected.size() == 10 && PageIconAssets().size() == expected.size());
    CHECK(mapped_files == expected && mapped_pages.size() == expected.size());
    CHECK(ordered_pages.size() == std::size(kPageOrder) && mapped_pages == ordered_pages);
    for (const std::string& name : expected) {
        CHECK(fs::is_regular_file(icons / name));
    }

    CHECK(PageIconAssets().size() == std::size(kPageOrder));
    for (const PageId page : kPageOrder) {
        const PageIconAsset* asset = FindPageIcon(page);
        CHECK(asset != nullptr);
        if (!asset) {
            continue;
        }
        const std::string svg = ReadAll(icons / asset->file);
        CHECK(svg.find("viewBox=\"") != std::string::npos && svg.find("<svg") != std::string::npos);
        CHECK(svg.find("filter") == std::string::npos && svg.find("fill=\"white\"") == std::string::npos);

        // The compiled copy is the file on disk, so a stale embed cannot pass.
        const std::vector<uint8_t> embedded = RenderPageIcon(page, 24);
        CHECK(!embedded.empty() && embedded == RasterizeIcon(reinterpret_cast<const uint8_t*>(svg.data()), svg.size(),
                                                              asset->optical_scale, 24));

        CHECK(asset->optical_scale > 0.5f && asset->optical_scale < 1.0f);
        for (const int pixels : {20, 30, 45}) {
            const std::vector<uint8_t> image = RasterizeIcon(reinterpret_cast<const uint8_t*>(svg.data()), svg.size(),
                                                             asset->optical_scale, pixels);
            CHECK(image.size() == size_t(pixels) * size_t(pixels) * 4);
            size_t covered = 0;
            bool edge = false, tinted = true;
            for (size_t i = 0; i + 3 < image.size(); i += 4) {
                const size_t x = (i / 4) % size_t(pixels), y = (i / 4) / size_t(pixels);
                const bool ink = image[i + 3] > 128;
                covered += ink;
                edge = edge || (ink && (x == 0 || y == 0 || x + 1 == size_t(pixels) || y + 1 == size_t(pixels)));
                tinted = tinted && image[i] == 255 && image[i + 1] == 255 && image[i + 2] == 255;
            }
            CHECK(covered > image.size() / 4 / 20);
            CHECK(!edge && tinted);
        }
    }

    size_t svg_files = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(icons)) {
        const std::string name = entry.path().filename().string();
        CHECK(name.find("svgrepo-com") == std::string::npos);
        svg_files += entry.path().extension() == ".svg";
    }
    CHECK(!fs::exists(icons / "bug-file-svgrepo-com.svg"));
    CHECK(svg_files == PageIconAssets().size());

    // A bad resource gives no image instead of a crash.
    const std::string broken = "<svg viewBox=\"0 0 24 24\"><path d=\"M";
    CHECK(RasterizeIcon(reinterpret_cast<const uint8_t*>(broken.data()), broken.size(), 0.9f, 20).empty());
    const std::string text = "not an svg";
    CHECK(RasterizeIcon(reinterpret_cast<const uint8_t*>(text.data()), text.size(), 0.9f, 20).empty());
    CHECK(RasterizeIcon(nullptr, 0, 0.9f, 20).empty());
    CHECK(RenderPageIcon(static_cast<PageId>(99), 20).empty());

    // One production path, keyed by page identity and compiled in, so packages carry no loose icons.
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    CHECK(launcher.find("c.pages.back().icon = [this, page](int pixels) { return PageIconTexture(page, pixels); };") != std::string::npos);
    CHECK(launcher.find("trg::PageIcon") == std::string::npos);
    const std::string compact = ReadAll(source / "gpu" / "patches" / "trg-launcher" / "0006-compact-layout.patch");
    CHECK(compact.find("DrawPageIcon") == std::string::npos && compact.find("PageIcon::") == std::string::npos);
    CHECK(compact.find("std::function<ImTextureID(int pixels)> icon = nullptr;") != std::string::npos);
    const std::string build = ReadAll(source / "launcher" / "CMakeLists.txt");
    CHECK(build.find("assets/launcher/icons/*.svg") != std::string::npos && build.find("-DSYMBOL=kPageIconFiles") != std::string::npos);
    const std::string windows = ReadAll(source / "tools" / "package.ps1");
    const std::string linux_package = ReadAll(source / "tools" / "package-linux.sh");
    CHECK(windows.find("launcher\\icons") == std::string::npos && linux_package.find("launcher/icons") == std::string::npos);

    // Nothing reads the source tree at run time: the icons are compiled in, only the artwork PNGs are loose files.
    const std::string loader = ReadAll(source / "launcher" / "page_icons.cpp");
    CHECK(loader.find("DCP_SOURCE_DIR") == std::string::npos && loader.find("ifstream") == std::string::npos);
    CHECK(loader.find("fopen") == std::string::npos && loader.find("SDL_GetBasePath") == std::string::npos);
    CHECK(loader.find("assets/launcher") == std::string::npos && loader.find("assets\\launcher") == std::string::npos);
    for (const char* file : {"launcher.cpp", "online_page.cpp", "trophy_page.cpp", "host.cpp"}) {
        const std::string text = ReadAll(source / "launcher" / file);
        CHECK(text.find("DCP_SOURCE_DIR") == std::string::npos && text.find("icons/") == std::string::npos);
        CHECK(text.find(".svg\"") == std::string::npos && text.find("nsvgParseFromFile") == std::string::npos);
    }
}

// ---- Launcher icon and third-party licenses ----

void TestIconLicenses() {
    const fs::path source(DCP_SOURCE_DIR);
    const fs::path icons = source / "assets" / "launcher" / "icons";
    const std::string sources = ReadAll(icons / "SOURCES.md");
    const std::string licenses = ReadAll(source / "packaging" / "licenses" / "THIRD-PARTY-LICENSES.txt");
    CHECK(!sources.empty() && !licenses.empty());

    int cc0_rows = 0;
    for (const PageIconAsset& asset : PageIconAssets()) {
        const size_t row = sources.find(std::string("| `") + asset.file + "` | ");
        CHECK(row != std::string::npos);
        const std::string line = sources.substr(row, sources.find('\n', row) - row);
        CHECK(line.find("https://www.svgrepo.com/svg/") != std::string::npos);
        CHECK(licenses.find(asset.file) != std::string::npos);
        if (asset.page == PageId::kDisplay) {
            CHECK(line.find("| CC Attribution License |") != std::string::npos && line.find("CC0") == std::string::npos);
            CHECK(line.find("nickylimyeanfen") != std::string::npos && line.find("Inc Device Icons") != std::string::npos);
            CHECK(line.find("https://www.svgrepo.com/svg/381247/display-television-tv-screen-gadget") != std::string::npos);
        } else if (asset.page == PageId::kBugReport) {
            CHECK(line.find("| Bug File | Vectopus | Atlas Variety Line Icons |") != std::string::npos);
            CHECK(line.find("https://www.svgrepo.com/svg/456346/bug-file") != std::string::npos);
            CHECK(line.find("| MIT License |") != std::string::npos);
        } else if (line.find("| CC0 |") != std::string::npos) {
            ++cc0_rows;
            const size_t url = line.find("https://www.svgrepo.com/svg/");
            const std::string address = line.substr(url, line.find(' ', url) - url);
            CHECK(licenses.find(address) != std::string::npos);
        }
    }
    CHECK(cc0_rows == 8);
    CHECK(licenses.find("CC0 (public domain dedication), all from SVG Repo") != std::string::npos);
    CHECK(licenses.find("\"Display Television Tv Screen Gadget\" by nickylimyeanfen, collection Inc Device Icons.") != std::string::npos);
    CHECK(licenses.find("License: CC Attribution License, https://creativecommons.org/licenses/by/") != std::string::npos);
    CHECK(licenses.find("\"Bug File\" by Vectopus, collection Atlas Variety Line Icons.") != std::string::npos);
    CHECK(licenses.find("https://www.svgrepo.com/svg/456346/bug-file") != std::string::npos);
    CHECK(licenses.find("bug-report.svg, MIT License:") != std::string::npos);
    CHECK(licenses.find("Copyright (c) Vectopus\n\nPermission is hereby granted") != std::string::npos);
    for (const std::string& attribution : {licenses, sources}) {
        CHECK(attribution.find("https://creativecommons.org/licenses/by/") != std::string::npos);
        CHECK(attribution.find("Adapted") != std::string::npos);
    }

    // NanoSVG stays vendored with its license, and the package carries that license.
    const fs::path nanosvg = source / "launcher" / "third_party" / "nanosvg";
    for (const char* file : {"nanosvg.h", "nanosvgrast.h", "LICENSE.txt"}) {
        CHECK(fs::is_regular_file(nanosvg / file) && fs::file_size(nanosvg / file) > 500);
    }
    const std::string nanosvg_license = ReadAll(nanosvg / "LICENSE.txt");
    CHECK(nanosvg_license.find("Mikko Mononen") != std::string::npos && nanosvg_license.find("as-is") != std::string::npos);
    CHECK(licenses.find("nanosvg ") != std::string::npos && licenses.find("Mikko Mononen") != std::string::npos);
    CHECK(licenses.find("https://github.com/memononen/nanosvg") != std::string::npos && licenses.find("licenses\\nanosvg") != std::string::npos);
    const std::string header = ReadAll(nanosvg / "nanosvg.h");
    CHECK(header.find("size == (size_t)-1l") != std::string::npos && header.find("size == -1l") == std::string::npos);

    const std::string windows = ReadAll(source / "tools" / "package.ps1");
    const std::string linux_package = ReadAll(source / "tools" / "package-linux.sh");
    CHECK(windows.find("launcher\\third_party\\nanosvg\\LICENSE.txt") != std::string::npos);
    CHECK(windows.find("packaging\\licenses\\THIRD-PARTY-LICENSES.txt") != std::string::npos);
    CHECK(linux_package.find("[nanosvg]=launcher/third_party/nanosvg/LICENSE.txt") != std::string::npos);
    CHECK(linux_package.find("packaging/licenses/THIRD-PARTY-LICENSES.txt") != std::string::npos);
}

// ---- Launcher page navigation ----

struct PageFrame {
    int sidebar;
    int content;
};

// One launcher frame: the commit comes first, the click arrives between the sidebar and the content.
PageFrame DrawPageFrame(trg::PageNavigation& navigation, int clicked) {
    navigation.Commit();
    const int sidebar = navigation.current();
    if (clicked >= 0) {
        navigation.Request(clicked);
    }
    return {sidebar, navigation.current()};
}

void TestPageNavigation() {
    enum { kPlay, kDisplay, kGraphics, kGameplay, kOnline };
    trg::PageNavigation navigation;

    const PageFrame clicked = DrawPageFrame(navigation, kDisplay);
    CHECK(clicked.sidebar == kPlay && clicked.content == kPlay);
    const PageFrame next = DrawPageFrame(navigation, -1);
    CHECK(next.sidebar == kDisplay && next.content == kDisplay);

    int shown = kDisplay;
    for (const int page : {kGraphics, kGameplay, kOnline, kPlay, kDisplay}) {
        const PageFrame frame = DrawPageFrame(navigation, page);
        CHECK(frame.sidebar == shown && frame.content == shown);
        shown = page;
    }

    trg::PageNavigation requests(kGameplay);
    CHECK(requests.current() == kGameplay && !requests.Commit());
    requests.Request(kPlay);
    requests.Request(kOnline);
    CHECK(requests.current() == kGameplay);
    CHECK(requests.Commit() && requests.current() == kOnline && !requests.Commit());
    requests.Request(kOnline);
    CHECK(requests.Commit());

    // Requests arriving at any point of any frame never leave the sidebar and the content on different pages.
    trg::PageNavigation rapid;
    uint32_t seed = 12345;
    int mixed = 0, changes = 0, last = rapid.current();
    for (int frame = 0; frame < 2000; ++frame) {
        seed = seed * 1664525u + 1013904223u;
        const int click = (seed >> 24) % 4 == 0 ? int((seed >> 8) % 9) : -1;
        const PageFrame drawn = DrawPageFrame(rapid, click);
        mixed += drawn.sidebar != drawn.content;
        changes += drawn.sidebar != last;
        last = drawn.sidebar;
    }
    CHECK(mixed == 0 && changes > 100);

    // The launcher commits once, before anything is drawn, and each page keeps its own scroll window. Row tables need
    // explicit weights: an automatic one collapses the label column on the table's first frame.
    const std::string trg = ReadAll(fs::path(DCP_SOURCE_DIR) / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    const size_t commit = trg.find("navigation_.Commit()");
    CHECK(commit != std::string::npos && trg.find("navigation_.Commit()", commit + 1) == std::string::npos);
    CHECK(commit < trg.find("DrawHeader(vp->Pos") && commit < trg.find("DrawSidebar(ImVec2(sidebar_w"));
    CHECK(trg.find("navigation_.Request(index);") != std::string::npos && trg.find("page_ = ") == std::string::npos);
    const size_t page_id = trg.find("ImGui::PushID(current_page);");
    CHECK(page_id != std::string::npos && page_id < trg.find("ImGui::BeginChild(\"##page_body\""));
    CHECK(trg.find("ImGui::BeginChild(\"##page_body\"") == trg.rfind("ImGui::BeginChild(\"##page_body\""));
    CHECK(trg.find("TableSetupColumn(\"label\", ImGuiTableColumnFlags_WidthStretch, 1.0f)") != std::string::npos);
    CHECK(trg.find("TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, std::max(width, 1.0f))") != std::string::npos);
}

// ---- Keyboard bindings ----

std::string KeyNameOf(int scancode) {
    char name[32];
    return dcp_key_name(scancode, name, sizeof(name));
}

void TestKeyBindings(const fs::path& base) {
    const fs::path file = base / "keys" / "dcp.ini";
    fs::remove_all(file.parent_path());
    fs::create_directories(file.parent_path());

    // The defaults are the layout the runtime had before the bindings were configurable.
    DcpKeyMap map;
    dcp_keys_defaults(&map);
    CHECK(map.scancode[DCP_KEY_LEFT_STICK_UP] == SDL_SCANCODE_W && map.scancode[DCP_KEY_LEFT_STICK_DOWN] == SDL_SCANCODE_S);
    CHECK(map.scancode[DCP_KEY_LEFT_STICK_LEFT] == SDL_SCANCODE_A && map.scancode[DCP_KEY_LEFT_STICK_RIGHT] == SDL_SCANCODE_D);
    CHECK(map.scancode[DCP_KEY_RIGHT_STICK_UP] == SDL_SCANCODE_UP && map.scancode[DCP_KEY_RIGHT_STICK_RIGHT] == SDL_SCANCODE_RIGHT);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_SPACE && map.scancode[DCP_KEY_CIRCLE] == SDL_SCANCODE_LSHIFT);
    CHECK(map.scancode[DCP_KEY_SQUARE] == SDL_SCANCODE_E && map.scancode[DCP_KEY_TRIANGLE] == SDL_SCANCODE_Q);
    CHECK(map.scancode[DCP_KEY_L1] == SDL_SCANCODE_1 && map.scancode[DCP_KEY_R1] == SDL_SCANCODE_3);
    CHECK(map.scancode[DCP_KEY_L2] == SDL_SCANCODE_R && map.scancode[DCP_KEY_R2] == SDL_SCANCODE_F);
    CHECK(map.scancode[DCP_KEY_L3] == SDL_SCANCODE_Z && map.scancode[DCP_KEY_R3] == SDL_SCANCODE_C);
    CHECK(map.scancode[DCP_KEY_DPAD_UP] == SDL_SCANCODE_I && map.scancode[DCP_KEY_DPAD_RIGHT] == SDL_SCANCODE_L);
    CHECK(map.scancode[DCP_KEY_OPTIONS] == SDL_SCANCODE_RETURN);
    CHECK(map.scancode[DCP_KEY_TOUCHPAD_LEFT] == SDL_SCANCODE_TAB && map.scancode[DCP_KEY_TOUCHPAD_RIGHT] == SDL_SCANCODE_BACKSPACE);
    CHECK(map.scancode[DCP_KEY_HOTKEY_CHEATS] == SDL_SCANCODE_F10 && map.scancode[DCP_KEY_HOTKEY_WINDOW_MODE] == SDL_SCANCODE_F11);
    for (int i = 0; i < DCP_KEY_COUNT; ++i) {
        CHECK(map.scancode[i] != SDL_SCANCODE_F12);
    }

    // Saved names: the shadPS4 ones, lowercase.
    CHECK(KeyNameOf(SDL_SCANCODE_SPACE) == "space" && KeyNameOf(SDL_SCANCODE_LSHIFT) == "lshift");
    CHECK(KeyNameOf(SDL_SCANCODE_F10) == "f10" && KeyNameOf(SDL_SCANCODE_UP) == "up" && KeyNameOf(SDL_SCANCODE_Q) == "q");
    CHECK(KeyNameOf(SDL_SCANCODE_RETURN) == "enter" && KeyNameOf(SDL_SCANCODE_PAGEUP) == "pgup");
    CHECK(KeyNameOf(SDL_SCANCODE_ESCAPE).empty() && KeyNameOf(SDL_SCANCODE_UNKNOWN).empty());
    for (const int scancode : {SDL_SCANCODE_A, SDL_SCANCODE_5, SDL_SCANCODE_F1, SDL_SCANCODE_F12, SDL_SCANCODE_TAB,
                               SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_DELETE, SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME,
                               SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RSHIFT,
                               SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT,
                               SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_SEMICOLON}) {
        CHECK(dcp_key_from_name(KeyNameOf(scancode).c_str()) == scancode);
    }

    // Parsing: case-insensitive, SDL's own names work too, anything else is refused.
    CHECK(dcp_key_from_name("SPACE") == SDL_SCANCODE_SPACE && dcp_key_from_name("Left Shift") == SDL_SCANCODE_LSHIFT);
    CHECK(dcp_key_from_name("F10") == SDL_SCANCODE_F10 && dcp_key_from_name("pgdown") == SDL_SCANCODE_PAGEDOWN);
    CHECK(dcp_key_from_name("") == 0 && dcp_key_from_name(nullptr) == 0 && dcp_key_from_name("notakey") == 0);
    CHECK(dcp_key_from_name("escape") == 0 && !dcp_key_bindable(SDL_SCANCODE_ESCAPE));

    // Reading the file: valid lines apply, invalid ones keep the default, other lines are ignored.
    {
        std::ofstream(file) << "# comment\nshow_launcher = 0\nkey_cross = Q\nkey_circle = notakey\nkey_l1 = \n"
                               "key_left_stick_up = up\nhotkey_window_mode = f9 # note\nkey_unknown = a\n";
    }
    dcp_keys_load(file.string().c_str(), &map);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_Q);
    CHECK(map.scancode[DCP_KEY_CIRCLE] == SDL_SCANCODE_LSHIFT && map.scancode[DCP_KEY_L1] == SDL_SCANCODE_1);
    CHECK(map.scancode[DCP_KEY_LEFT_STICK_UP] == SDL_SCANCODE_UP && map.scancode[DCP_KEY_LEFT_STICK_DOWN] == SDL_SCANCODE_S);
    CHECK(map.scancode[DCP_KEY_HOTKEY_WINDOW_MODE] == SDL_SCANCODE_F9 && map.scancode[DCP_KEY_HOTKEY_CHEATS] == SDL_SCANCODE_F10);

    // The same key on two controls is allowed: both read it.
    {
        std::ofstream(file) << "key_cross = e\n";
    }
    dcp_keys_load(file.string().c_str(), &map);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_E && map.scancode[DCP_KEY_SQUARE] == SDL_SCANCODE_E);

    // A missing file and a config without any binding (an older build's) are the defaults.
    dcp_keys_load((file.parent_path() / "missing.ini").string().c_str(), &map);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_SPACE);
    {
        std::ofstream(file) << "show_launcher = 0\nvsync = 1\n";
    }
    dcp_keys_load(file.string().c_str(), &map);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_SPACE && map.scancode[DCP_KEY_HOTKEY_CHEATS] == SDL_SCANCODE_F10);
    {
        const LauncherSettings settings(file);
        for (int i = 0; i < DCP_KEY_COUNT; ++i) {
            CHECK(SavedKey(settings, static_cast<DcpKeyAction>(i)) == dcp_key_actions[i].default_scancode);
        }
    }

    // Persistence: the launcher writes names, the runtime and the video core read them.
    {
        LauncherSettings settings(file);
        settings.Set(kVSyncKey, "0");
        SetKey(settings, DCP_KEY_CROSS, SDL_SCANCODE_J);
        SetKey(settings, DCP_KEY_LEFT_STICK_UP, SDL_SCANCODE_KP_8);
        SetKey(settings, DCP_KEY_HOTKEY_CHEATS, SDL_SCANCODE_F9);
        SetKey(settings, DCP_KEY_CIRCLE, SDL_SCANCODE_ESCAPE); // not bindable: ignored
        CHECK(settings.Save());
    }
    const std::string written = ReadAll(file);
    CHECK(WrittenValue(written, "key_cross", "j") && WrittenValue(written, "key_left_stick_up", "kp8"));
    CHECK(WrittenValue(written, "hotkey_cheats_menu", "f9") && written.find("key_circle") == std::string::npos);
    {
        const LauncherSettings settings(file);
        CHECK(SavedKey(settings, DCP_KEY_CROSS) == SDL_SCANCODE_J && SavedKey(settings, DCP_KEY_CIRCLE) == SDL_SCANCODE_LSHIFT);
        CHECK(KeyDisplayName(SavedKey(settings, DCP_KEY_LEFT_STICK_UP)) == "Keypad 8");
        CHECK(KeyDisplayName(SDL_SCANCODE_RETURN) == "Enter" && KeyDisplayName(SDL_SCANCODE_SPACE) == "Space");
    }
    dcp_keys_load(file.string().c_str(), &map);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_J && map.scancode[DCP_KEY_LEFT_STICK_UP] == SDL_SCANCODE_KP_8);
    DcpSettings::Values values;
    CHECK(values.menu_key == SDL_SCANCODE_F10 && values.window_mode_key == SDL_SCANCODE_F11);
    CHECK(DcpSettings::Read(file.string(), values));
    CHECK(values.menu_key == SDL_SCANCODE_F9 && values.window_mode_key == SDL_SCANCODE_F11 && !values.vsync);

    // A bad saved name is the default for the launcher too.
    CHECK(IsValidSetting("key_cross", "q") && !IsValidSetting("key_cross", "notakey") && !IsValidSetting("hotkey_window_mode", "escape"));

    // Reset puts back the keys alone.
    {
        LauncherSettings settings(file);
        ResetKeys(settings);
        CHECK(settings.Save());
        for (int i = 0; i < DCP_KEY_COUNT; ++i) {
            CHECK(SavedKey(settings, static_cast<DcpKeyAction>(i)) == dcp_key_actions[i].default_scancode);
        }
    }
    CHECK(WrittenValue(ReadAll(file), "key_cross", "space") && WrittenValue(ReadAll(file), "vsync", "0"));
    dcp_keys_load(file.string().c_str(), &map);
    CHECK(map.scancode[DCP_KEY_CROSS] == SDL_SCANCODE_SPACE && map.scancode[DCP_KEY_HOTKEY_CHEATS] == SDL_SCANCODE_F10);

    // Every action has a managed setting with its default name.
    for (const DcpKeyInfo& action : dcp_key_actions) {
        bool managed = false;
        for (const ManagedSetting& setting : ManagedSettings()) {
            managed |= setting.key == action.ini_key && setting.default_value == KeyNameOf(action.default_scancode);
        }
        CHECK(managed);
    }
}

// ---- Controller prompts: block patches on synthetic CPK archives ----

namespace cpk_test {

void Be16(std::string& o, uint32_t v) { o += char(v >> 8); o += char(v); }
void Be32(std::string& o, uint32_t v) { for (int i = 3; i >= 0; --i) o += char(v >> (8 * i)); }
void Be64(std::string& o, uint64_t v) { for (int i = 7; i >= 0; --i) o += char(v >> (8 * i)); }
void Le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += char(v >> (8 * i)); }
void Le64(std::string& o, uint64_t v) { for (int i = 0; i < 8; ++i) o += char(v >> (8 * i)); }

struct Column {
    const char* name;
    int type;    // @UTF type: 2 u16, 4 u32, 6 u64, 0xA string
    int storage; // 0x50 per row, 0x30 constant
};
struct Cell {
    uint64_t number;
    std::string text;
};
using Row = std::vector<std::pair<std::string, Cell>>;

Cell N(uint64_t n) { return {n, {}}; }
Cell S(std::string s) { return {0, std::move(s)}; }

// A @UTF table packet, as scripts/prompts/cpk.py's self-test builds it.
std::string Utf(const std::string& name, const std::vector<Column>& columns, const std::vector<Row>& rows) {
    std::string strings("<NULL>", 7);
    const auto intern = [&](const std::string& text) {
        const uint32_t off = uint32_t(strings.size());
        strings += text;
        strings += '\0';
        return off;
    };
    const auto cell = [](const Row& row, const char* key) {
        for (const auto& [k, v] : row) {
            if (k == key) return v;
        }
        return Cell{};
    };
    const uint32_t name_off = intern(name);
    std::string cols;
    for (const Column& c : columns) {
        cols += char(c.storage | c.type);
        Be32(cols, intern(c.name));
        if (c.storage == 0x30) {
            const uint64_t v = cell(rows.at(0), c.name).number;
            if (c.type == 2) Be16(cols, uint32_t(v)); else if (c.type == 4) Be32(cols, uint32_t(v)); else Be64(cols, v);
        }
    }
    std::string rowbuf;
    uint32_t row_len = 0;
    for (const Row& row : rows) {
        const size_t start = rowbuf.size();
        for (const Column& c : columns) {
            if (c.storage != 0x50) continue;
            const Cell v = cell(row, c.name);
            if (c.type == 0xA) Be32(rowbuf, intern(v.text));
            else if (c.type == 2) Be16(rowbuf, uint32_t(v.number));
            else if (c.type == 4) Be32(rowbuf, uint32_t(v.number));
            else Be64(rowbuf, v.number);
        }
        row_len = uint32_t(rowbuf.size() - start);
    }
    const uint32_t rows_off = 32 + uint32_t(cols.size()) - 8;
    const uint32_t strings_off = rows_off + uint32_t(rowbuf.size());
    std::string packet("@UTF");
    Be32(packet, 0);
    Be32(packet, rows_off);
    Be32(packet, strings_off);
    Be32(packet, strings_off + uint32_t(strings.size()));
    Be32(packet, name_off);
    Be16(packet, uint32_t(columns.size()));
    Be16(packet, row_len);
    Be32(packet, uint32_t(rows.size()));
    packet += cols + rowbuf + strings;
    const uint32_t size = uint32_t(packet.size() - 8);
    for (int i = 0; i < 4; ++i) packet[4 + i] = char(size >> (8 * (3 - i)));
    return packet;
}

// CRI's XOR, its own inverse.
std::string Crypt(std::string packet) {
    uint32_t m = 0x5F;
    for (char& b : packet) {
        b = char(uint8_t(b) ^ uint8_t(m));
        m = (m * 0x15) & 0xFF;
    }
    return packet;
}

std::string Section(const char* magic, const std::string& packet) {
    std::string out(magic, 4);
    Le32(out, 0xFF);
    Le64(out, packet.size());
    return out + packet;
}

// A CRILAYLA block storing `payload` with literal bytes only: the stream is read backwards and writes the output
// backwards, and the file's first 0x100 bytes are stored raw after it.
std::string CrilaylaLiterals(const std::string& payload) {
    std::string out;
    uint64_t acc = 0;
    int bits = 0;
    for (size_t i = payload.size(); i > 0x100; --i) {
        acc = (acc << 9) | uint8_t(payload[i - 1]);
        bits += 9;
        while (bits >= 8) {
            bits -= 8;
            out += char(acc >> bits);
        }
        acc &= (1u << bits) - 1;
    }
    if (bits) out += char(acc << (8 - bits));
    std::string block("CRILAYLA");
    Le32(block, uint32_t(payload.size() - 0x100));
    Le32(block, uint32_t(out.size()));
    block.append(out.rbegin(), out.rend());
    return block + payload.substr(0, 0x100);
}

struct Entry {
    std::string path;
    std::string data;
    bool compressed;
};

// A CPK with an encrypted header (EnableTocCrc on), an optionally encrypted TOC and 2 KB alignment, as the game's.
std::string BuildCpk(const std::vector<Entry>& entries, bool encrypt_toc) {
    const uint64_t toc_offset = 0x800, content_offset = 0x3000, align = 0x800;
    std::string content;
    std::vector<Row> rows;
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string stored = entries[i].compressed ? CrilaylaLiterals(entries[i].data) : entries[i].data;
        while ((content_offset + content.size()) % align) content += '\0';
        const size_t slash = entries[i].path.rfind('/');
        rows.push_back({{"DirName", S(entries[i].path.substr(0, slash))},
                        {"FileName", S(entries[i].path.substr(slash + 1))},
                        {"FileSize", N(stored.size())},
                        {"ExtractSize", N(entries[i].data.size())},
                        {"FileOffset", N(content_offset + content.size() - toc_offset)},
                        {"ID", N(i)},
                        {"UserString", S("<NULL>")}});
        content += stored;
    }
    std::string toc = Utf("CpkTocInfo",
                          {{"DirName", 0xA, 0x50}, {"FileName", 0xA, 0x50}, {"FileSize", 4, 0x50}, {"ExtractSize", 4, 0x50},
                           {"FileOffset", 6, 0x50}, {"ID", 4, 0x50}, {"UserString", 0xA, 0x50}},
                          rows);
    if (encrypt_toc) toc = Crypt(toc);
    const std::string header = Utf("CpkHeader",
                                   {{"TocOffset", 6, 0x50}, {"ContentOffset", 6, 0x50}, {"Files", 4, 0x50}, {"Align", 2, 0x50},
                                    {"EnableTocCrc", 2, 0x50}, {"EnableFileCrc", 2, 0x50}, {"Version", 2, 0x30}},
                                   {{{"TocOffset", N(toc_offset)}, {"ContentOffset", N(content_offset)}, {"Files", N(entries.size())},
                                     {"Align", N(align)}, {"EnableTocCrc", N(1)}, {"EnableFileCrc", N(0)}, {"Version", N(7)}}});
    std::string blob = Section("CPK ", Crypt(header));
    blob.resize(toc_offset, '\0');
    blob += Section("TOC ", toc);
    blob.resize(content_offset, '\0');
    return blob + content;
}

// A DCPX patch turning `before` into `after` (same size), as scripts/prompts/make_dcpx.py writes it.
std::string MakePatch(const std::vector<std::pair<std::string, std::pair<std::string, std::string>>>& targets) {
    std::string out("DCPX1\0\0\0", 8);
    Le32(out, uint32_t(targets.size()));
    for (const auto& [name, versions] : targets) {
        const auto& [before, after] = versions;
        std::vector<size_t> runs;
        for (size_t i = 0; i + 16 <= before.size(); i += 16) {
            if (before.compare(i, 16, after, i, 16) != 0) runs.push_back(i);
        }
        out += char(name.size() & 255);
        out += char(name.size() >> 8);
        out += name;
        Le32(out, uint32_t(before.size()));
        Le32(out, Crc32({reinterpret_cast<const uint8_t*>(before.data()), before.size()}));
        Le32(out, Crc32({reinterpret_cast<const uint8_t*>(after.data()), after.size()}));
        Le32(out, uint32_t(runs.size()));
        for (const size_t off : runs) {
            Le32(out, uint32_t(off));
            out += after.substr(off, 16);
        }
    }
    return out;
}

std::string Bytes(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }
std::span<const uint8_t> Span(const std::string& s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

} // namespace cpk_test

void TestControllerPrompts(const fs::path& base) {
    using namespace cpk_test;
    // The setting and its labels.
    CHECK(ParsePromptStyle("xbox") == PromptStyle::kXbox && ParsePromptStyle("playstation") == PromptStyle::kPlayStation);
    CHECK(!ParsePromptStyle("nintendo") && std::string(PromptStyleKey(PromptStyle::kXbox)) == "xbox");
    CHECK(IsValidSetting(kControllerPromptsKey, "xbox") && !IsValidSetting(kControllerPromptsKey, "Xbox"));
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kControllerPromptsKey && setting.default_value == kPromptsPlayStation;
    }
    CHECK(managed);

    // The shipped patch parses and names the button atlas packs of every language (the plain pack and the English one,
    // then the German, Spanish, French, Italian and UK packs, which hold the same bytes as the English one), and the
    // translated message sheets whose touchpad wording it changes.
    {
        const std::string shipped = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "prompts" / "xbox-prompts.dcpx");
        std::string error;
        const std::optional<BlockPatch> patch = ParseBlockPatch(Span(shipped), &error);
        const std::vector<std::string> atlas_names = {
            "stage/Interface.ftx",    "stage/Interface_US.ftx", "stage/Interface_DE.ftx", "stage/Interface_ES.ftx",
            "stage/Interface_FR.ftx", "stage/Interface_IT.ftx", "stage/Interface_UK.ftx"};
        const std::vector<std::string> message_names = {"msgsheet/SystemMsg_DE.fms", "msgsheet/SystemMsg_ES.fms",
                                                        "msgsheet/SystemMsg_FR.fms", "msgsheet/SystemMsg_IT.fms"};
        CHECK(patch && patch->targets.size() == atlas_names.size() + message_names.size());
        if (patch && patch->targets.size() == atlas_names.size() + message_names.size()) {
            const BlockPatchTarget& us = patch->targets[1];
            for (size_t i = 0; i < atlas_names.size(); ++i) {
                const BlockPatchTarget& target = patch->targets[i];
                CHECK(target.name == atlas_names[i]);
                CHECK(target.size == (i == 0 ? 31461008u : 35655696u));
                CHECK(target.runs.size() > 10000);
                CHECK(i == 0 || (target.runs.size() == us.runs.size() && target.crc_before == us.crc_before));
            }
            for (size_t i = 0; i < message_names.size(); ++i) {
                const BlockPatchTarget& target = patch->targets[atlas_names.size() + i];
                CHECK(target.name == message_names[i]);
                CHECK(!target.runs.empty() && target.runs.size() < 100);
            }
        }
        CHECK(!ParseBlockPatch(Span(shipped.substr(0, 100)), &error) && error.find("truncated") != std::string::npos);
        CHECK(!ParseBlockPatch(Span(std::string("hello")), &error));
    }

    // Two archives holding fake button atlas packs: 20 KB of pattern, one stored raw, one CRILAYLA-compressed.
    std::string iface(20480, '\0'), iface_us(24576, '\0');
    for (size_t i = 0; i < iface.size(); ++i) iface[i] = char((i * 7 + i / 64) & 255);
    for (size_t i = 0; i < iface_us.size(); ++i) iface_us[i] = char((i * 13 + i / 32) & 255);
    std::string want = iface, want_us = iface_us;
    for (size_t off : {0x340u, 0x1000u, 0x4ff0u}) want.replace(off, 16, 16, 'X');
    for (size_t off : {0x380u, 0x2000u}) want_us.replace(off, 16, 16, 'Y');
    const std::string patch_bytes = MakePatch({{"stage/Interface.ftx", {iface, want}}, {"stage/Interface_US.ftx", {iface_us, want_us}}});
    const std::string fms = "FMSB" + std::string(600, '\0');
    const fs::path app0 = base / "prompts_app0";
    fs::create_directories(app0);
    const std::string original_us = BuildCpk({{"msgsheet/x.fms", fms, true}, {"stage/Interface.ftx", iface, false},
                                              {"stage/Interface_US.ftx", iface_us, true}}, true);
    const std::string original = BuildCpk({{"stage/Interface.ftx", iface, true}}, false);
    WriteAll(app0 / "DRACRO_US.CPK", original_us);
    WriteAll(app0 / "DRACRO.CPK", original);
    CHECK(PromptArchives(app0, kUs).size() == 2 && InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kPlayStation);

    // Reading: header, TOC (encrypted), case-insensitive lookup, CRILAYLA.
    {
        CpkArchive archive;
        std::string error;
        CHECK(archive.Open(app0 / "DRACRO_US.CPK", &error));
        CHECK(archive.entries().size() == 3 && archive.align() == 0x800 && archive.enable_toc_crc() == 1);
        CHECK(archive.Find("STAGE/interface_us.ftx") != nullptr && !archive.Find("stage/missing.ftx"));
        CHECK(Bytes(archive.Read(*archive.Find("msgsheet/x.fms"))) == fms);
        CHECK(Bytes(archive.Read(*archive.Find("stage/Interface.ftx"))) == iface);
        CHECK(Bytes(archive.Read(*archive.Find("stage/Interface_US.ftx"))) == iface_us);
        CHECK(archive.Find("stage/Interface_US.ftx")->size != iface_us.size()); // stored as a CRILAYLA stream
        CHECK(archive.Find("stage/Interface.ftx")->size == iface.size() && !archive.Find("stage/Interface.ftx")->compressed());
        CpkArchive not_cpk;
        CHECK(!not_cpk.Open(app0 / "missing.CPK", &error));
        WriteAll(app0 / "text.CPK", "not an archive at all, just text");
        CHECK(!not_cpk.Open(app0 / "text.CPK", &error) && !error.empty());
        fs::remove(app0 / "text.CPK");
    }

    // Xbox: both archives patched, entries re-read as patched, uncompressed, the CRC flag cleared, the setting
    // file untouched (the launcher saves it).
    std::vector<std::string> phases;
    PatchHooks hooks;
    hooks.phase = [&phases](const char* text) { phases.emplace_back(text); };
    PromptsResult r = SetPromptStyle(app0, kUs, PromptStyle::kXbox, Span(patch_bytes), hooks);
    CHECK(r.ok && !r.cancelled);
    CHECK(InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kXbox);
    CHECK(fs::exists(UndoPath(app0 / "DRACRO_US.CPK")) && fs::exists(UndoPath(app0 / "DRACRO.CPK")));
    CHECK(!phases.empty() && phases[0] == "Patching DRACRO_US.CPK...");
    {
        CpkArchive archive;
        CHECK(archive.Open(app0 / "DRACRO_US.CPK"));
        CHECK(archive.enable_toc_crc() == 0);
        CHECK(Bytes(archive.Read(*archive.Find("stage/Interface.ftx"))) == want);
        CHECK(Bytes(archive.Read(*archive.Find("stage/Interface_US.ftx"))) == want_us);
        CHECK(Bytes(archive.Read(*archive.Find("msgsheet/x.fms"))) == fms);
        CHECK(archive.Find("stage/Interface_US.ftx")->size == want_us.size() && !archive.Find("stage/Interface_US.ftx")->compressed());
        CHECK(archive.Find("stage/Interface_US.ftx")->offset >= original_us.size());
        CHECK(archive.Find("stage/Interface_US.ftx")->offset % 0x800 == 0);
        CpkArchive other;
        CHECK(other.Open(app0 / "DRACRO.CPK") && Bytes(other.Read(*other.Find("stage/Interface.ftx"))) == want);
        // the original data is still there, untouched, before the appended copies
        CHECK(ReadAll(app0 / "DRACRO_US.CPK").compare(0x3000, original_us.size() - 0x3000, original_us, 0x3000, std::string::npos) == 0);
    }
    // Choosing Xbox again changes nothing.
    r = SetPromptStyle(app0, kUs, PromptStyle::kXbox, Span(patch_bytes));
    CHECK(r.ok && r.detail.find("already patched") != std::string::npos);
    const auto aligned = [](size_t n) { return (n + 0x7FF) / 0x800 * 0x800; };
    CHECK(fs::file_size(app0 / "DRACRO_US.CPK") == aligned(aligned(original_us.size()) + iface.size()) + iface_us.size());

    // PlayStation: byte-identical archives, undo records gone.
    r = SetPromptStyle(app0, kUs, PromptStyle::kPlayStation, {});
    CHECK(r.ok);
    CHECK(ReadAll(app0 / "DRACRO_US.CPK") == original_us && ReadAll(app0 / "DRACRO.CPK") == original);
    CHECK(InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kPlayStation && !fs::exists(UndoPath(app0 / "DRACRO.CPK")));
    r = SetPromptStyle(app0, kUs, PromptStyle::kPlayStation, {});
    CHECK(r.ok && r.detail.find("not patched") != std::string::npos);

    // A half-patched install reads as PlayStation; Xbox completes it, PlayStation cleans it up.
    CHECK(ApplyBlockPatch(app0 / "DRACRO.CPK", *ParseBlockPatch(Span(patch_bytes))).ok);
    CHECK(InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kPlayStation);
    CHECK(SetPromptStyle(app0, kUs, PromptStyle::kXbox, Span(patch_bytes)).ok && InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kXbox);
    CHECK(SetPromptStyle(app0, kUs, PromptStyle::kPlayStation, {}).ok && ReadAll(app0 / "DRACRO.CPK") == original);

    // Cancelling while writing leaves the archive as it was.
    hooks.phase = nullptr;
    hooks.progress = [](uint64_t, uint64_t) { return false; };
    r = SetPromptStyle(app0, kUs, PromptStyle::kXbox, Span(patch_bytes), hooks);
    CHECK(!r.ok && r.cancelled);
    CHECK(ReadAll(app0 / "DRACRO_US.CPK") == original_us && InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kPlayStation);

    // An archive whose entry is not the file the patch was made for is skipped and named; others are patched.
    std::string foreign = iface;
    foreign[0x2000] = char(foreign[0x2000] + 1);
    WriteAll(app0 / "DRACRO.CPK", BuildCpk({{"stage/Interface.ftx", foreign, false}}, false));
    r = SetPromptStyle(app0, kUs, PromptStyle::kXbox, Span(patch_bytes));
    CHECK(r.ok && r.detail.find("stage/Interface.ftx is not the file the patch was made for") != std::string::npos);
    CHECK(InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kPlayStation); // DRACRO.CPK stayed original: no undo record
    CHECK(ArchiveState(app0 / "DRACRO_US.CPK") == ArchivePatchState::kPatched);
    CHECK(SetPromptStyle(app0, kUs, PromptStyle::kPlayStation, {}).ok && ReadAll(app0 / "DRACRO_US.CPK") == original_us);

    // An unreadable patch is refused before anything is touched; a folder without archives too.
    r = SetPromptStyle(app0, kUs, PromptStyle::kXbox, Span(std::string("DCPX1")));
    CHECK(!r.ok && r.detail.find("unreadable") != std::string::npos && InstalledPromptStyle(app0, kUs, Span(patch_bytes)) == PromptStyle::kPlayStation);
    CHECK(!SetPromptStyle(base / "nowhere", kUs, PromptStyle::kXbox, Span(patch_bytes)).ok);
    CHECK(PromptArchives(base / "nowhere", kUs).empty() && InstalledPromptStyle(base / "nowhere", kUs, Span(patch_bytes)) == PromptStyle::kPlayStation);

    // Failures and interruptions leave the archive usable: it is put back, or recognised and redone.
    {
        const fs::path dir = base / "prompts_safety";
        const fs::path us = dir / "DRACRO_US.CPK";
        WriteAll(us, original_us);
        const BlockPatch parsed = *ParseBlockPatch(Span(patch_bytes));
        PatchHooks cancel_writing;
        cancel_writing.progress = [](uint64_t, uint64_t total) { return total == 0; };
        const PatchResult cancelled = ApplyBlockPatch(us, parsed, cancel_writing);
        CHECK(!cancelled.ok && cancelled.cancelled && ReadAll(us) == original_us && !fs::exists(UndoPath(us)));

        // Cut off after the undo record and before the table of contents.
        CHECK(ApplyBlockPatch(us, parsed).ok);
        WriteAll(us, original_us);
        CHECK(ArchiveState(us) == ArchivePatchState::kPatched);
        const PatchResult redone = ApplyBlockPatch(us, parsed);
        CpkArchive redone_archive;
        CHECK(redone.ok && redone.patched == 2 && redone_archive.Open(us) &&
              Bytes(redone_archive.Read(*redone_archive.Find("stage/Interface_US.ftx"))) == want_us);
        CHECK(RevertBlockPatch(us).ok && ReadAll(us) == original_us);

        // An undo record that is not this archive's, or is garbage, is refused and nothing is written.
        CHECK(ApplyBlockPatch(us, parsed).ok);
        const std::string patched_bytes = ReadAll(us);
        const std::string other = BuildCpk({{"stage/Interface.ftx", foreign, false}}, false);
        WriteAll(us, other);
        const PatchResult not_ours = RevertBlockPatch(us);
        CHECK(!not_ours.ok && not_ours.detail.find("not for this archive") != std::string::npos && ReadAll(us) == other);
        WriteAll(us, patched_bytes);
        WriteAll(UndoPath(us), "DCPUNDO1 not a record");
        CHECK(!RevertBlockPatch(us).ok && ReadAll(us) == patched_bytes);
        fs::remove(UndoPath(us));

        // Entries pointing past the end of a truncated archive are a read error.
        WriteAll(dir / "cut.CPK", original_us.substr(0, 0x3000));
        CpkArchive cut;
        std::string error;
        const CpkEntry* missing = cut.Open(dir / "cut.CPK", &error) ? cut.Find("stage/Interface.ftx") : nullptr;
        CHECK(missing && cut.Read(*missing, &error).empty() && !error.empty());

        // An archive hard-linked to the folder the install was made from is patched on a copy of its own.
        const fs::path shared = base / "prompts_shared", linked = base / "prompts_linked";
        WriteAll(shared / "DRACRO_US.CPK", original_us);
        fs::create_directories(linked);
        std::error_code ec;
        fs::create_hard_link(shared / "DRACRO_US.CPK", linked / "DRACRO_US.CPK", ec);
        if (!ec) {
            CHECK(SetPromptStyle(linked, kUs, PromptStyle::kXbox, Span(patch_bytes)).ok);
            CHECK(InstalledPromptStyle(linked, kUs, Span(patch_bytes)) == PromptStyle::kXbox && ReadAll(shared / "DRACRO_US.CPK") == original_us);
            CHECK(fs::hard_link_count(linked / "DRACRO_US.CPK") == 1 && !fs::exists(linked / "DRACRO_US.CPK.copy"));
            CHECK(SetPromptStyle(linked, kUs, PromptStyle::kPlayStation, {}).ok && ReadAll(linked / "DRACRO_US.CPK") == original_us);
        }

        // An update staged over a patched base starts from the original archive, and the base keeps its patch.
        const fs::path patched_base = base / "update_base", staged = base / "update_staged", update_files = base / "update_files";
        WriteAll(patched_base / "DRACRO_US.CPK", original_us);
        CHECK(ApplyBlockPatch(patched_base / "DRACRO_US.CPK", parsed).ok);
        const std::string patched_base_bytes = ReadAll(patched_base / "DRACRO_US.CPK");
        CHECK(patched_base_bytes != original_us);
        fs::create_directories(update_files);
        CHECK(StageUpdate(patched_base, update_files, staged).ok());
        CHECK(ReadAll(staged / "DRACRO_US.CPK") == original_us && !fs::exists(UndoPath(staged / "DRACRO_US.CPK")));
        CHECK(ReadAll(patched_base / "DRACRO_US.CPK") == patched_base_bytes && fs::exists(UndoPath(patched_base / "DRACRO_US.CPK")));
    }

    // An install patched by an earlier patch, which lacks a language pack, reads as PlayStation until the newer patch is
    // applied, and then holds that pack too.
    {
        const fs::path upgrade = base / "prompts_upgrade";
        fs::create_directories(upgrade);
        WriteAll(upgrade / "DRACRO_US.CPK",
                 BuildCpk({{"stage/Interface_US.ftx", iface_us, true}, {"stage/Interface_ES.ftx", iface_us, true}}, true));
        const std::string earlier = MakePatch({{"stage/Interface_US.ftx", {iface_us, want_us}}});
        const std::string later =
            MakePatch({{"stage/Interface_US.ftx", {iface_us, want_us}}, {"stage/Interface_ES.ftx", {iface_us, want_us}}});
        CHECK(ApplyBlockPatch(upgrade / "DRACRO_US.CPK", *ParseBlockPatch(Span(earlier))).ok);
        CHECK(InstalledPromptStyle(upgrade, kUs, Span(earlier)) == PromptStyle::kXbox);
        CHECK(InstalledPromptStyle(upgrade, kUs, Span(later)) == PromptStyle::kPlayStation);
        CHECK(SetPromptStyle(upgrade, kUs, PromptStyle::kXbox, Span(later)).ok &&
              InstalledPromptStyle(upgrade, kUs, Span(later)) == PromptStyle::kXbox);
        CpkArchive archive;
        CHECK(archive.Open(upgrade / "DRACRO_US.CPK") && Bytes(archive.Read(*archive.Find("stage/Interface_ES.ftx"))) == want_us);
    }

    // An install patched by a patch that held an entry the current one does not is stale: it is restored to the original
    // and patched again, so the dropped entry is not left behind and the undo record keeps the original length.
    {
        const fs::path stale = base / "prompts_stale";
        fs::create_directories(stale);
        const std::string archive_bytes =
            BuildCpk({{"stage/Interface_US.ftx", iface_us, true}, {"msgsheet/SystemMsg_DE.fms", fms, true}}, true);
        WriteAll(stale / "DRACRO_US.CPK", archive_bytes);
        std::string fms_patched = fms;
        fms_patched.replace(96, 16, 16, 'T');
        const std::string earlier = MakePatch(
            {{"stage/Interface_US.ftx", {iface_us, want_us}}, {"msgsheet/SystemMsg_DE.fms", {fms, fms_patched}}});
        const std::string later = MakePatch({{"stage/Interface_US.ftx", {iface_us, want_us}}});
        CHECK(ApplyBlockPatch(stale / "DRACRO_US.CPK", *ParseBlockPatch(Span(earlier))).ok);
        CHECK(InstalledPromptStyle(stale, kUs, Span(earlier)) == PromptStyle::kXbox);
        CHECK(InstalledPromptStyle(stale, kUs, Span(later)) == PromptStyle::kPlayStation);
        CHECK(SetPromptStyle(stale, kUs, PromptStyle::kXbox, Span(later)).ok &&
              InstalledPromptStyle(stale, kUs, Span(later)) == PromptStyle::kXbox);
        CpkArchive archive;
        CHECK(archive.Open(stale / "DRACRO_US.CPK"));
        CHECK(Bytes(archive.Read(*archive.Find("msgsheet/SystemMsg_DE.fms"))) == fms);
        CHECK(Bytes(archive.Read(*archive.Find("stage/Interface_US.ftx"))) == want_us);
        CHECK(SetPromptStyle(stale, kUs, PromptStyle::kPlayStation, {}).ok && ReadAll(stale / "DRACRO_US.CPK") == archive_bytes);
    }

    // An update's archives are offered Xbox only while they hold every file the patch was made for; the base build keeps
    // its partial behaviour. Patching the update never writes the base's archives it is linked to.
    {
        const fs::path base_game = base / "prompts_update_base", update_game = base / "prompts_update";
        fs::remove_all(base_game);
        fs::remove_all(update_game);
        WriteAll(base_game / "DRACRO_US.CPK", original_us);
        WriteAll(base_game / "DRACRO.CPK", original);
        fs::create_directories(update_game);
        std::error_code ec;
        fs::create_hard_link(base_game / "DRACRO.CPK", update_game / "DRACRO.CPK", ec);
        if (ec) {
            fs::copy_file(base_game / "DRACRO.CPK", update_game / "DRACRO.CPK");
        }
        WriteAll(update_game / "DRACRO_US.CPK", original_us);
        const BlockPatch parsed_patch = *ParseBlockPatch(Span(patch_bytes));
        std::string why;
        CHECK(BaseBuildOf(kUs101) == &kUs && PromptPatchFits(update_game, kUs101, parsed_patch, &why));
        CHECK(XboxPromptsAvailable(update_game, kUs101, Span(patch_bytes)) && XboxPromptsAvailable(base_game, kUs, Span(patch_bytes)));
        CHECK(SetPromptStyle(update_game, kUs101, PromptStyle::kXbox, Span(patch_bytes)).ok);
        CHECK(InstalledPromptStyle(update_game, kUs101, Span(patch_bytes)) == PromptStyle::kXbox);
        CHECK(XboxPromptsAvailable(update_game, kUs101, Span(patch_bytes)));
        CHECK(ReadAll(base_game / "DRACRO.CPK") == original && ReadAll(base_game / "DRACRO_US.CPK") == original_us);
        CHECK(fs::hard_link_count(update_game / "DRACRO.CPK") == 1 && fs::hard_link_count(base_game / "DRACRO.CPK") == 1);
        CHECK(SetPromptStyle(update_game, kUs101, PromptStyle::kPlayStation, {}).ok);
        CHECK(ReadAll(update_game / "DRACRO_US.CPK") == original_us && ReadAll(update_game / "DRACRO.CPK") == original);

        // The update replaced one of the patch's files: nothing is offered or written, on any archive.
        WriteAll(update_game / "DRACRO_US.CPK", BuildCpk({{"msgsheet/x.fms", fms, true}, {"stage/Interface.ftx", foreign, false},
                                                          {"stage/Interface_US.ftx", iface_us, true}}, true));
        const std::string foreign_us = ReadAll(update_game / "DRACRO_US.CPK");
        CHECK(!PromptPatchFits(update_game, kUs101, parsed_patch, &why));
        CHECK(why == "stage/Interface.ftx in DRACRO_US.CPK is not the file the patch was made for");
        CHECK(!XboxPromptsAvailable(update_game, kUs101, Span(patch_bytes)));
        const PromptsResult refused = SetPromptStyle(update_game, kUs101, PromptStyle::kXbox, Span(patch_bytes));
        CHECK(!refused.ok && refused.detail == "Xbox prompts are not available for CUSA10454 1.01 USA");
        CHECK(!SetGamePatches(update_game, kUs101, PromptStyle::kXbox, false, Span(patch_bytes), {}).ok);
        CHECK(ReadAll(update_game / "DRACRO_US.CPK") == foreign_us && ReadAll(update_game / "DRACRO.CPK") == original);
        CHECK(!fs::exists(UndoPath(update_game / "DRACRO_US.CPK")) && !fs::exists(UndoPath(update_game / "DRACRO.CPK")));

        // A file the patch changes is missing from every archive.
        WriteAll(update_game / "DRACRO_US.CPK", BuildCpk({{"msgsheet/x.fms", fms, true}, {"stage/Interface.ftx", iface, false}}, true));
        CHECK(!PromptPatchFits(update_game, kUs101, parsed_patch, &why) && why == "stage/Interface_US.ftx is in none of the archives");

        // An entry an earlier patch rewrote fits: the revert puts the original back before the patch is applied.
        WriteAll(update_game / "DRACRO_US.CPK", original_us);
        std::string older_us = iface_us;
        older_us.replace(0x100, 16, 16, 'O');
        CHECK(ApplyBlockPatch(update_game / "DRACRO_US.CPK", *ParseBlockPatch(Span(MakePatch({{"stage/Interface_US.ftx", {iface_us, older_us}}})))).ok);
        CHECK(PromptPatchFits(update_game, kUs101, parsed_patch, &why));
        CHECK(SetGamePatches(update_game, kUs101, PromptStyle::kXbox, false, Span(patch_bytes), {}).ok);
        CHECK(InstalledPromptStyle(update_game, kUs101, Span(patch_bytes)) == PromptStyle::kXbox);
        CHECK(SetGamePatches(update_game, kUs101, PromptStyle::kPlayStation, false, {}, {}).ok);
        CHECK(ReadAll(update_game / "DRACRO_US.CPK") == original_us && ReadAll(base_game / "DRACRO.CPK") == original);
    }

    // CRILAYLA: a corrupt block is an error, not a crash.
    {
        std::string block = CrilaylaLiterals(iface);
        block.resize(block.size() - 0x100 - 10);
        std::string error;
        CHECK(CrilaylaDecompress(Span(block), &error).empty() && !error.empty());
        CHECK(CrilaylaDecompress(Span(std::string("CRILAYLA")), &error).empty());
    }
}

} // namespace

namespace language_test {

using cpk_test::Entry;

std::string Sheet(const std::string& text) {
    return "FMSB" + std::string(0x1c, '\0') + text + std::string(300, '\0');
}

// One language slot: its message sheets and stage files, as DRACRO_US.CPK names them.
std::vector<Entry> Slot(const std::string& suffix, const std::string& system_msg) {
    return {{"msgsheet/SystemMsg_" + suffix + ".fms", system_msg, true},
            {"msgsheet/ItemList_" + suffix + ".fms", "items " + suffix, false},
            {"stage/Menu_" + suffix + ".ftx", "menu " + suffix, false},
            {"stage/Menu_" + suffix + ".mbs", "menu layout " + suffix, false}};
}

std::string LanguageArchive(std::initializer_list<std::vector<Entry>> slots) {
    std::vector<Entry> entries{{"other/US_manual_001.ftx", "manual", false}, {"stage/Interface.ftx", "plain", false}};
    for (const std::vector<Entry>& one : slots) {
        entries.insert(entries.end(), one.begin(), one.end());
    }
    return cpk_test::BuildCpk(entries, true);
}

const std::string kEnglishSheet = Sheet("New Game Continue Options Quit @#(34) ##(TOUCH)");
const std::string kRussianSheet = Sheet("Новая игра Продолжить Выход Настройки Сохранить ##(TOUCH) @#(34)");

} // namespace language_test

// Menu & Subtitles languages read from the installed regional archive: complete language file sets, never a language
// the build does not list, and the build's own list when the archive cannot tell.
void TestInstalledTextLanguages(const fs::path& base) {
    using namespace cpk_test;
    using namespace language_test;
    const std::string& english = kEnglishSheet;
    const std::string& russian = kRussianSheet;
    const std::string ukrainian = Sheet("Нова гра Продовжити Вихід Налаштування Зберегти їжа є ґанок ##(TOUCH)");

    std::vector<Entry> us = Slot("US", english);
    us.push_back({"msgsheet/StaffRollMsg_PS4_US.fms", "staff", false}); // the US slot's own extra sheet
    const std::vector<Entry> fr = Slot("FR", english), de = Slot("DE", english), it = Slot("IT", english);
    const std::vector<Entry> es = Slot("ES", english), uk = Slot("UK", english);
    const fs::path app0 = base / "languages_app0";
    fs::remove_all(app0);
    fs::create_directories(app0);
    const fs::path regional = app0 / "DRACRO_US.CPK";
    const auto keys = [&](const GameBuild* build) {
        std::string joined;
        for (const TextLanguage& language : InstalledTextLanguages(app0, build)) {
            if (!language.translation.empty()) continue;
            joined += (joined.empty() ? "" : " ") + std::string(language.key) + "=" + std::to_string(language.ps4_id);
        }
        return joined;
    };
    const std::string western = "english=1 french=2 german=4 italian=5 spanish=3";
    const std::string& us_languages = western; // the native slots: Brazilian Portuguese follows the English one

    // Every Western slot (the official 1.00 archive's layout): all five, in the build's order, on 1.00 and 1.01 alike;
    // both add Brazilian Portuguese over the English slot.
    WriteAll(regional, LanguageArchive({us, fr, de, it, es, uk}));
    CHECK(keys(&kUs) == us_languages && keys(&kUs101) == us_languages);
    CHECK(SameLanguages(InstalledTextLanguages(app0, &kUs), TextLanguagesOf(&kUs)));

    // The reported 1.01 archive, Russian text in the US slot and no other slot, offers English and no native Russian: the
    // US slot keeps its system language. Russian is the port's translation over that slot, as on every build.
    WriteAll(regional, LanguageArchive({Slot("US", russian), uk}));
    const std::vector<TextLanguage> russian_slot = InstalledTextLanguages(app0, &kUs101);
    CHECK(keys(&kUs101) == "english=1" && russian_slot[0].name == "English" && russian_slot[0].suffix == "US");
    CHECK(russian_slot.size() == 3 && russian_slot[1].key == kPortugueseBrazilKey && russian_slot[2].key == kRussianKey);
    CHECK(russian_slot.size() == 3 && russian_slot[2].suffix == "US" && russian_slot[2].translation == kPortRussian);
    for (const char* saved : {"english", "french", "german", "italian", "spanish"}) {
        CHECK(ResolveTextLanguage(russian_slot, saved).key == "english");
    }
    CHECK(ResolveTextLanguage(russian_slot, "russian").key == "russian");
    CHECK(UnavailableLanguageNote("korean", russian_slot[0]) == "saved language Korean unavailable for this build, using English");
    CHECK(UnavailableLanguageNote("english", russian_slot[0]).empty() && UnavailableLanguageNote("", russian_slot[0]).empty());
    CHECK(UnavailableLanguageNote("klingon", TextLanguagesOf(&kUs)[0]) ==
          "saved language klingon unavailable for this build, using English");

    const fs::path ini = base / "languages_dcp.ini";
    const fs::path image = base / "languages_image";
    WriteAll(image / "image.json", std::string("{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"") +
                                       kUs101.eboot_sha256 + "\",\n  \"app_ver\": \"01.01\"\n}");
    CHECK(BuildOfImageDir(image.string().c_str()) == &kUs101);
    for (const char* saved :
         {"english", "french", "german", "italian", "spanish", "russian", "korean", "traditional_chinese", "japanese", ""}) {
        WriteAll(ini, std::string("text_language=") + saved + "\n");
        CHECK(dcp_text_language(ini.string().c_str(), image.string().c_str(), app0.string().c_str()) == 1);
    }
    fs::remove(ini);
    CHECK(dcp_text_language(ini.string().c_str(), image.string().c_str(), app0.string().c_str()) == 1);
    // Without the folder the build's own list stands, as before.
    WriteAll(ini, "text_language=french\n");
    CHECK(dcp_text_language(ini.string().c_str(), image.string().c_str(), nullptr) == 2);
    CHECK(IsValidSetting(kTextLanguageKey, "russian") && !IsValidSetting(kTextLanguageKey, "Russian"));
    CHECK(IsValidSetting(kTextLanguageKey, "english") && IsValidSetting(kTextLanguageKey, "traditional_chinese"));
    // A saved Russian, from an older launcher too, is the port's Russian on every build, played in its slot.
    CHECK(ResolveTextLanguage(TextLanguagesOf(&kCk), "russian").key == "russian");
    WriteAll(regional, LanguageArchive({us, fr, de, it, es}));
    CHECK(ResolveTextLanguage(InstalledTextLanguages(app0, &kUs), "russian").key == "russian");
    WriteAll(ini, "text_language=russian\n");
    CHECK(dcp_text_language(ini.string().c_str(), image.string().c_str(), app0.string().c_str()) == 1);

    // Russian text in the US slot beside complete Western slots: still the Western languages, and 1.00 still offers its
    // translation (TranslateArchive keeps any record whose text differs).
    WriteAll(regional, LanguageArchive({Slot("US", russian), fr, de, it, es}));
    CHECK(keys(&kUs101) == western);
    CHECK(keys(&kUs) == us_languages);
    WriteAll(ini, "text_language=german\n");
    CHECK(dcp_text_language(ini.string().c_str(), image.string().c_str(), app0.string().c_str()) == 4);

    // The install log describes the slots with counts and checksums, never with text.
    const auto contains = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    const std::string described = DescribeLanguageResources(app0, &kUs101);
    CHECK(described.starts_with("DRACRO_US.CPK: 22 entries") && contains(described, "; US 4 files, SystemMsg crc "));
    CHECK(contains(described, "; FR 4 files, SystemMsg crc ") && contains(described, "; DE 4 files, SystemMsg crc "));
    CHECK(described.ends_with("; offered English, French, German, Italian, Portuguese (Brazil), Russian, Spanish"));
    CHECK(!contains(described, "Новая") && DescribeLanguageResources(app0, &kUs101) == described);
    WriteAll(regional, LanguageArchive({us}));
    CHECK(DescribeLanguageResources(app0, &kUs101).ends_with("; offered English, Portuguese (Brazil), Russian"));
    WriteAll(regional, "not an archive");
    CHECK(DescribeLanguageResources(app0, &kUs101) == "DRACRO_US.CPK unreadable");
    fs::remove(regional);
    CHECK(DescribeLanguageResources(app0, &kUs101) == "DRACRO_US.CPK absent");
    CHECK(DescribeLanguageResources(app0, &kJp103).empty() && DescribeLanguageResources(app0, nullptr).empty());
    WriteAll(regional, LanguageArchive({Slot("US", russian), fr, de, it, es}));

    // A slot missing a file most slots have is not offered; a slot with nothing at all is not either.
    std::vector<Entry> french_without_menu = Slot("FR", english);
    french_without_menu.erase(french_without_menu.begin() + 2);
    WriteAll(regional, LanguageArchive({us, french_without_menu, de, it}));
    CHECK(keys(&kUs101) == "english=1 german=4 italian=5");
    WriteAll(ini, "text_language=french\n");
    CHECK(dcp_text_language(ini.string().c_str(), image.string().c_str(), app0.string().c_str()) == 1);

    // Cyrillic text in a slot never renames or adds a language.
    WriteAll(regional, LanguageArchive({Slot("US", russian), Slot("FR", russian)}));
    CHECK(keys(&kUs101) == "english=1 french=2");
    WriteAll(regional, LanguageArchive({Slot("US", ukrainian)}));
    CHECK(keys(&kUs101) == "english=1");

    // The archive cannot tell: unreadable, holding no language slot, or missing. The build's own list stands.
    WriteAll(regional, "not an archive");
    CHECK(keys(&kUs101) == western);
    WriteAll(regional, BuildCpk({{"stage/Interface.ftx", "plain", false}}, false));
    CHECK(keys(&kUs101) == western);
    fs::remove(regional);
    CHECK(keys(&kUs) == us_languages && keys(&kUs101) == western && keys(nullptr) == western);
    CHECK(InstalledTextLanguages({}, &kUs101).size() == 7);

    // Asia/Hong Kong: Traditional Chinese and Korean from DRACRO_CK.CPK, unchanged.
    WriteAll(app0 / "DRACRO_CK.CPK", LanguageArchive({Slot("CT", Sheet("繁體中文")), Slot("KR", Sheet("한국어"))}));
    CHECK(keys(&kCk100) == "traditional_chinese=10 korean=9" && keys(&kCk) == "traditional_chinese=10 korean=9");
    WriteAll(app0 / "DRACRO_CK.CPK", LanguageArchive({Slot("CT", Sheet("繁體中文"))}));
    CHECK(keys(&kCk) == "traditional_chinese=10");

    // Japan: Japanese only, whatever the patch archive holds, even a Western slot the executable cannot select.
    for (const GameBuild* japan : {&kJp100, &kJp102, &kJp103}) {
        CHECK(keys(japan) == "japanese=0");
    }
    WriteAll(app0 / "DRACRO_PATCH_JP.CPK", BuildCpk({{"stage/patch.bin", "patch", false}}, false));
    CHECK(keys(&kJp102) == "japanese=0" && keys(&kJp103) == "japanese=0");
    WriteAll(app0 / "DRACRO_PATCH_JP.CPK", LanguageArchive({us, fr}));
    CHECK(keys(&kJp102) == "japanese=0" && keys(&kJp103) == "japanese=0");

    // Every archive language names its own slot; Japanese has none and a translation the slot of a language before it.
    for (const GameBuild& build : SupportedBuilds()) {
        std::set<std::string_view> suffixes;
        for (const TextLanguage& language : TextLanguagesOf(&build)) {
            CHECK(language.key == "japanese"                                   ? language.suffix.empty()
                  : !language.translation.empty() && !language.suffix.empty()  ? suffixes.count(language.suffix) == 1
                  : !language.translation.empty()                              ? std::string_view(build.title_id) == "CUSA07001"
                                                  : language.suffix.size() == 2 && suffixes.insert(language.suffix).second);
        }
    }
    fs::remove_all(app0);
    fs::remove_all(image);
    fs::remove(ini);
}

// English on Japan and Asia/Hong Kong: the port's built-in corpus, served over the slot with the slot's own text kept
// where a record cannot map. No USA game, no user copy and no PT-BR letters or art.
void TestBuiltInEnglish(const fs::path& base) {
    CHECK(EnglishTranslationFiles().size() == 13);
    CHECK(EnglishSource("US") && EnglishSource("JP") && EnglishSource("CT") && !EnglishSource("KR"));

    struct Real {
        const GameBuild* build;
        const char* folder;
        size_t applied;
    };
    const fs::path ini = base / "english_builtin.ini", image = base / "english_builtin_image";
    for (const Real& item : {Real{&kJp100, "CUSA07001-1.00", 11971}, Real{&kJp102, "CUSA07001-1.02", 11971},
                             Real{&kJp103, "CUSA07001-1.03", 11971}, Real{&kCk100, "CUSA10209-1.00", 11686}}) {
        const fs::path game = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / item.folder;
        if (!fs::exists(game / item.build->translation->archive)) continue;
        WriteAll(image / "image.json", std::string("{\n  \"title_id\": \"") + item.build->title_id +
                                           "\",\n  \"eboot_sha256\": \"" + item.build->eboot_sha256 + "\",\n  \"app_ver\": \"" +
                                           FinishedLayer(*item.build).app_ver + "\"\n}");
        WriteAll(ini, "text_language=english\n");
        CHECK(dcp_text_font_probe(ini.string().c_str(), image.string().c_str(), game.string().c_str()) == 0);
        uint64_t length = 0;
        DcpFilePatch* patches = nullptr;
        size_t count = 0;
        char text[1024];
        int composed = 1;
        const char* archive = dcp_text_translation(ini.string().c_str(), image.string().c_str(), game.string().c_str(),
                                                   &length, &patches, &count, text, sizeof(text), 1, &composed);
        const std::string message = text;
        std::printf("ENGLISH-SERVED|%s|%s\n", item.build->label, message.c_str());
        CHECK(archive && std::string_view(archive) == item.build->translation->archive && count > 0 && composed == 0);
        CHECK(message.find("English: " + std::to_string(item.applied) + " records translated of 11992") !=
              std::string::npos);
        CHECK(message.find("accented") == std::string::npos && message.find(", 0 visual replacements") != std::string::npos);
        CHECK(!localization::PortugueseBrazilSelected());
        CHECK(!dcp_text_stage_overlay(&length, &patches, &count));
        size_t glyphs = 1;
        int cell = 0;
        CHECK(!dcp_text_composed_glyphs(&glyphs, &cell) && glyphs == 0);

        for (const int character : {0, 1, 2, 3, 4, 5}) {
            for (const DefaultMessageCategory category : {DEFAULT_MESSAGE_GREETING, DEFAULT_MESSAGE_SELECTION,
                                                          DEFAULT_MESSAGE_RESURRECTION, DEFAULT_MESSAGE_DEATH}) {
                for (int variant = 0; variant < DEFAULT_MESSAGE_VARIANTS; ++variant) {
                    const int record = default_message_record(character, category, variant);
                    const char* message = dcp_text_default_bone(unsigned(record));
                    CHECK(record >= 1641 && message &&
                          std::all_of(message, message + std::strlen(message),
                                      [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
                }
            }
        }

        size_t system_sheets = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto* bytes = static_cast<const uint8_t*>(patches[i].data);
            const std::optional<FmsSheet> sheet = ParseFms(std::vector<uint8_t>(bytes, bytes + patches[i].size));
            if (!sheet || sheet->records.size() != EXIT_GAME_SYSTEM_MSG_COUNT) continue;
            ++system_sheets;
            CHECK(sheet->records[382] == "Start Game" && sheet->records[385] == "Options" &&
                  sheet->records[386] == "Gallery");
            CHECK(sheet->records[1444] ==
                  "The ##(COLOR_TOWN_MENU) allows you to select a\ntown facility and quickly move about.");
        }
        CHECK(system_sheets == 1);

        WriteAll(ini, std::string("text_language=") + std::string(item.build->text_languages[0].key) + "\n");
        CHECK(!dcp_text_translation(ini.string().c_str(), image.string().c_str(), game.string().c_str(), &length, &patches,
                                    &count, text, sizeof(text), 1, &composed));
        CHECK(!dcp_text_default_bone(1642));
    }
    localization::SetActiveLanguage("english");
    fs::remove_all(image);
    fs::remove(ini);
}

std::string Utf8Of(uint16_t code) {
    if (code < 0x80) return std::string(1, char(code));
    if (code < 0x800) return {char(0xC0 | code >> 6), char(0x80 | (code & 0x3F))};
    return {char(0xE0 | code >> 12), char(0x80 | (code >> 6 & 0x3F)), char(0x80 | (code & 0x3F))};
}

bool HasCyrillic(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) { return c == '\xD0' || c == '\xD1'; });
}

// Russian on every build: the fan translation's corpus served over the build's own slot, its letters drawn into the main
// font (over the fan package's kana on USA, in blank cells on Japan and Asia/Hong Kong) and the Exit Game rows matching.
void TestBuiltInRussian(const fs::path& base) {
    CHECK(RussianTranslationFiles().size() == 13);
    CHECK(RussianSource("JP") && RussianSource("CT") && !RussianSource("US"));
    const TranslationFile* glyph_file = RussianGlyphs();
    CHECK(glyph_file != nullptr);
    if (!glyph_file) return;
    const std::optional<std::vector<CyrillicGlyph>> glyphs = ParseCyrillicGlyphs({glyph_file->data, glyph_file->size});
    CHECK(glyphs && glyphs->size() == CYRILLIC_GLYPH_COUNT);
    if (!glyphs) return;
    std::set<uint16_t> codes;
    for (const CyrillicGlyph& glyph : *glyphs) codes.insert(glyph.code);
    CHECK(codes.size() == CYRILLIC_GLYPH_COUNT && codes.count(0x401) && codes.count(0x451));
    for (uint16_t code = 0x410; code < 0x450; ++code) CHECK(codes.count(code) == 1);
    std::vector<uint8_t> broken(glyph_file->data, glyph_file->data + glyph_file->size);
    broken[broken.size() / 2] ^= 0xFF;
    CHECK(!ParseCyrillicGlyphs(broken));

    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        CHECK(CyrillicKana(Utf8Of(cyrillic_letter(i))) == Utf8Of(cyrillic_kana(i)));
        CHECK(cyrillic_cell(CYRILLIC_CELLS_KANA, i) == (i < 33 ? 300 + i : 327 + i));
        CHECK(cyrillic_cell(CYRILLIC_CELLS_FREE, i) == LATIN_FIRST_CELL + LATIN_GLYPH_COUNT + i);
    }
    CHECK(cyrillic_cell(CYRILLIC_CELLS_FREE, CYRILLIC_GLYPH_COUNT - 1) < LATIN_ATLAS_COLUMNS * LATIN_ATLAS_COLUMNS);
    CHECK(CyrillicKana("Press @#(35) for %d％ ##(HDD).") == "Press @#(35) for %d％ ##(HDD).");
    CHECK(CyrillicKana("") == "" && !HasCyrillic(CyrillicKana("Выйти из игры.")));

    const ExitGameLanguage* russian = nullptr;
    for (const ExitGameLanguage& language : exit_game_languages) {
        if (language.key == kRussianKey) russian = &language;
    }
    CHECK(russian && russian->kana_cyrillic);
    if (!russian) return;

    struct Real {
        const GameBuild* build;
        const char* folder;
        size_t applied;
        CyrillicCells cells;
    };
    const fs::path ini = base / "russian_builtin.ini", image = base / "russian_builtin_image";
    for (const Real& item : {Real{&kUs, "v1.03", 9946, CYRILLIC_CELLS_KANA}, Real{&kJp100, "CUSA07001-1.00", 9926, CYRILLIC_CELLS_FREE},
                             Real{&kJp102, "CUSA07001-1.02", 9926, CYRILLIC_CELLS_FREE},
                             Real{&kJp103, "CUSA07001-1.03", 9926, CYRILLIC_CELLS_FREE},
                             Real{&kCk100, "CUSA10209-1.00", 9791, CYRILLIC_CELLS_FREE}}) {
        const fs::path game = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / item.folder;
        if (!fs::exists(game / item.build->translation->archive)) continue;
        WriteAll(image / "image.json", std::string("{\n  \"title_id\": \"") + item.build->title_id +
                                           "\",\n  \"eboot_sha256\": \"" + item.build->eboot_sha256 + "\",\n  \"app_ver\": \"" +
                                           FinishedLayer(*item.build).app_ver + "\"\n}");
        const std::string ini_path = ini.string(), image_path = image.string(), game_path = game.string();
        WriteAll(ini, "text_language=russian\n");
        unsigned probe = 1;
        CHECK(dcp_text_cyrillic_cells(ini_path.c_str(), image_path.c_str(), game_path.c_str(), &probe) == item.cells);
        CHECK(probe == item.build->translation->cyrillic.probe && probe != 0);
        CHECK(dcp_text_font_probe(ini_path.c_str(), image_path.c_str(), game_path.c_str()) == 0);
        uint64_t length = 0;
        DcpFilePatch* patches = nullptr;
        size_t count = 0;
        char text[1024];
        int composed = 1;
        CHECK(!dcp_text_translation(ini_path.c_str(), image_path.c_str(), game_path.c_str(), &length, &patches, &count, text,
                                    sizeof(text), 0, &composed) && composed == 0);
        CHECK(std::string(text).find("Russian unavailable") == 0);
        size_t metric_count = 1;
        CHECK(!dcp_text_cyrillic_metrics(&metric_count) && metric_count == 0);

        const char* archive = dcp_text_translation(ini_path.c_str(), image_path.c_str(), game_path.c_str(), &length,
                                                   &patches, &count, text, sizeof(text), 1, &composed);
        const std::string message = text;
        std::printf("RUSSIAN-SERVED|%s|%s\n", item.build->label, message.c_str());
        CHECK(archive && std::string_view(archive) == item.build->translation->archive && composed == 1);
        CHECK(message.find("Russian: " + std::to_string(item.applied) + " records translated of 9946 in 13 sheets") == 0);
        CHECK(message.find("Cyrillic letters drawn into the font") != std::string::npos);
        CHECK(!localization::PortugueseBrazilSelected() && !dcp_text_stage_overlay(&length, &patches, &count));
        const CyrillicMetrics* metrics = dcp_text_cyrillic_metrics(&metric_count);
        CHECK(metrics && metric_count == CYRILLIC_GLYPH_COUNT);
        for (size_t i = 0; metrics && i < metric_count; ++i) {
            CHECK(metrics[i].width == (*glyphs)[i].metrics.width && metrics[i].spacing == (*glyphs)[i].metrics.spacing);
        }
        size_t latin = 1;
        int cell_pixels = 0;
        CHECK(!dcp_text_composed_glyphs(&latin, &cell_pixels) && latin == 0);

        for (int character = 0; character < DEFAULT_MESSAGE_CLASSES; ++character) {
            for (const DefaultMessageCategory category : {DEFAULT_MESSAGE_GREETING, DEFAULT_MESSAGE_SELECTION,
                                                          DEFAULT_MESSAGE_RESURRECTION, DEFAULT_MESSAGE_DEATH}) {
                for (int variant = 0; variant < DEFAULT_MESSAGE_VARIANTS; ++variant) {
                    const char* line = dcp_text_default_bone(unsigned(default_message_record(character, category, variant)));
                    CHECK(line && HasCyrillic(line));
                }
            }
        }

        const bool kana = item.cells == CYRILLIC_CELLS_KANA;
        const auto served = [&](const char* unicode) { return kana ? CyrillicKana(unicode) : std::string(unicode); };
        size_t system_sheets = 0, fonts = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto* bytes = static_cast<const uint8_t*>(patches[i].data);
            std::vector<uint8_t> data(bytes, bytes + patches[i].size);
            if (data.size() == 16777680) {
                ++fonts;
                std::vector<uint8_t> expected = data;
                for (int letter = 0; letter < CYRILLIC_GLYPH_COUNT; ++letter) {
                    CHECK(WriteFontCell(expected, cyrillic_cell(item.cells, letter), (*glyphs)[size_t(letter)].blocks));
                }
                CHECK(expected == data);
                continue;
            }
            const std::optional<FmsSheet> sheet = ParseFms(data);
            if (!sheet || sheet->records.size() != EXIT_GAME_SYSTEM_MSG_COUNT) continue;
            ++system_sheets;
            CHECK(sheet->records[385] == served("Настройки") && sheet->records[386] == served("Галерея"));
            CHECK(sheet->records[EXIT_GAME_INSTALL_RECORD] == served(russian->install_original));
            CHECK(sheet->records[EXIT_GAME_HELP_RECORD] == served(russian->help_original));
            CHECK(sheet->records[848] == "Введите текст"); // drawn by the system font: Unicode on every build
            CHECK(kana != HasCyrillic(sheet->records[385]));
        }
        CHECK(system_sheets == 1 && fonts == 1);

        for (const std::string& other : {std::string(item.build->text_languages[0].key), std::string(kPortugueseBrazilKey)}) {
            WriteAll(ini, "text_language=" + other + "\n");
            CHECK(dcp_text_cyrillic_cells(ini_path.c_str(), image_path.c_str(), game_path.c_str(), &probe) == CYRILLIC_CELLS_NONE &&
                  probe == 0);
            dcp_text_translation(ini_path.c_str(), image_path.c_str(), game_path.c_str(), &length, &patches, &count, text,
                                 sizeof(text), 1, &composed);
            CHECK(!dcp_text_cyrillic_metrics(&metric_count) && metric_count == 0);
            CHECK(std::string(text).find("Russian") == std::string::npos);
        }
    }
    localization::SetActiveLanguage("english");
    fs::remove_all(image);
    fs::remove(ini);
}

// The Japanese builds offer Japanese, English, Portuguese (Brazil) and Russian; moving between installed builds replaces a saved
// language the new build has not got with its first one: in the launcher's file and in what the game is told.
void TestTextLanguageSwitching(const fs::path& base) {
    using namespace language_test;
    const fs::path root = base / "language_switch";
    fs::remove_all(root);
    const fs::path ini = root / "dcp.ini";
    const auto app0_of = [&](const GameBuild& build) { return root / "prepared" / build.folder; };
    const auto image_of = [&](const GameBuild& build) {
        const fs::path dir = root / "image" / build.folder;
        WriteAll(dir / "image.json", std::string("{\n  \"title_id\": \"") + build.title_id + "\",\n  \"eboot_sha256\": \"" +
                                         build.eboot_sha256 + "\",\n  \"app_ver\": \"" + FinishedLayer(build).app_ver +
                                         "\"\n}");
        return dir.string();
    };
    const std::vector<Entry> us = Slot("US", kEnglishSheet);
    WriteAll(app0_of(kUs) / "DRACRO_US.CPK", LanguageArchive({us, Slot("FR", kEnglishSheet), Slot("DE", kEnglishSheet),
                                                              Slot("IT", kEnglishSheet), Slot("ES", kEnglishSheet)}));
    WriteAll(app0_of(kUs101) / "DRACRO_US.CPK", LanguageArchive({Slot("US", kRussianSheet)}));
    WriteAll(app0_of(kCk) / "DRACRO_CK.CPK", LanguageArchive({Slot("CT", Sheet("CT")), Slot("KR", Sheet("KR"))}));
    WriteAll(app0_of(kJp102) / "DRACRO_PATCH_JP.CPK", cpk_test::BuildCpk({{"stage/bg32a.ftx", "patch", false}}, false));
    WriteAll(app0_of(kJp103) / "DRACRO_PATCH_JP.CPK", LanguageArchive({us}));
    fs::create_directories(app0_of(kJp100));
    const auto told = [&](const GameBuild& build, const std::string& saved) {
        WriteAll(ini, "text_language=" + saved + "\n");
        return dcp_text_language(ini.string().c_str(), image_of(build).c_str(), app0_of(build).string().c_str());
    };

    for (const GameBuild* japan : {&kJp100, &kJp102, &kJp103}) {
        const std::vector<TextLanguage> languages = InstalledTextLanguages(app0_of(*japan), japan);
        CHECK(languages.size() == 4 && languages[0].key == "japanese" && languages[0].name == "Japanese");
        CHECK(languages[1].key == kEnglishKey && languages[2].key == kPortugueseBrazilKey && languages[2].ps4_id == 0);
        CHECK(languages.size() == 4 && languages[3].key == kRussianKey && languages[3].ps4_id == 0);
        CHECK(NativeJapaneseNpcVoices(japan) && languages[0].ps4_id == 0 && SameLanguages(languages, TextLanguagesOf(japan)));
        for (const char* saved : {"japanese", "english", "french", "korean", "traditional_chinese", "russian", "", "x"}) {
            CHECK(told(*japan, saved) == 0);
        }
        CHECK(UnavailableLanguageNote("english", languages[0]) ==
              "saved language English unavailable for this build, using Japanese");
        CHECK(UnavailableLanguageNote("korean", languages[0]) ==
              "saved language Korean unavailable for this build, using Japanese");
        CHECK(UnavailableLanguageNote("russian", languages[0]) ==
              "saved language Russian unavailable for this build, using Japanese");
    }
    // Japanese reaches no other build: US and 1.01 send it to their US slot, Asia to Traditional Chinese.
    CHECK(told(kUs, "japanese") == 1 && told(kUs101, "japanese") == 1 && told(kCk, "japanese") == 10);

    const auto switch_to = [&](const GameBuild& build) {
        LauncherSettings settings(ini);
        NormalizeTextLanguage(settings, InstalledTextLanguages(app0_of(build), &build));
        CHECK(settings.Save());
        return LauncherSettings(ini).Get(kTextLanguageKey);
    };
    WriteAll(ini, "text_language=japanese\n");
    CHECK(switch_to(kUs) == "english");             // Japan -> USA
    CHECK(switch_to(kJp100) == "english");          // USA -> Japan: English is listed there too
    CHECK(switch_to(kCk) == "english");             // Japan -> Asia
    WriteAll(ini, "text_language=korean\n");
    CHECK(switch_to(kJp102) == "japanese");         // Asia -> Japan
    WriteAll(ini, "text_language=korean\n");
    CHECK(switch_to(kUs) == "english");             // Asia -> USA
    CHECK(switch_to(kUs101) == "english");          // USA -> 1.01
    CHECK(switch_to(kJp103) == "english");          // 1.01 -> Japan
    WriteAll(ini, "text_language=russian\n");
    CHECK(switch_to(kUs) == "russian" && switch_to(kUs101) == "russian" && switch_to(kJp100) == "russian");
    CHECK(switch_to(kCk) == "russian");
    CHECK(told(kUs, "russian") == 1 && told(kCk, "russian") == 10 && told(kJp103, "russian") == 0);
    CHECK(told(kUs, "brazilian_portuguese") == 1 && told(kCk, "brazilian_portuguese") == 10);
    for (const GameBuild* japan : {&kJp100, &kJp102, &kJp103}) CHECK(told(*japan, "brazilian_portuguese") == 0);
    // The saved language survives every switch between supported builds.
    WriteAll(ini, "text_language=brazilian_portuguese\n");
    for (const GameBuild* build : {&kUs101, &kJp100, &kCk, &kJp102, &kUs, &kJp103, &kUs101}) {
        CHECK(switch_to(*build) == "brazilian_portuguese");
    }
    for (const char* kept : {"english", "french", "german", "italian", "brazilian_portuguese", "russian", "spanish"}) {
        WriteAll(ini, std::string("text_language=") + kept + "\n");
        LauncherSettings settings(ini);
        CHECK(!NormalizeTextLanguage(settings, InstalledTextLanguages(app0_of(kUs), &kUs)));
        CHECK(settings.Get(kTextLanguageKey) == kept);
    }
    WriteAll(ini, "show_fps=1\n");
    {
        LauncherSettings settings(ini);
        CHECK(!NormalizeTextLanguage(settings, InstalledTextLanguages(app0_of(kJp100), &kJp100)));
        CHECK(!settings.Has(kTextLanguageKey));
    }

    // English is listed and ready without another game or a user copy. The same key plays native English on USA and
    // Europe and the built-in corpus on Japan and Asia/Hong Kong, through every switch.
    const GameBuild* const port_english[] = {&kJp100, &kJp102, &kJp103, &kCk100, &kCk};
    const auto offered = [&](const GameBuild& build) { return InstalledTextLanguages(app0_of(build), &build); };
    const auto english_in = [](const std::vector<TextLanguage>& languages) {
        return std::count_if(languages.begin(), languages.end(), [](const TextLanguage& l) { return l.key == kEnglishKey; });
    };
    for (const GameBuild* build : port_english) {
        const std::vector<TextLanguage> languages = offered(*build);
        CHECK(english_in(languages) == 1 && SameLanguages(languages, TextLanguagesOf(build)));
        CHECK(OffersPortEnglish(build));
    }
    for (const GameBuild* build : {&kUs, &kUs101}) {
        CHECK(english_in(offered(*build)) == 1 && !OffersPortEnglish(build));
    }
    const std::vector<TextLanguage> asia_list = offered(kCk);
    CHECK(asia_list.size() == 5 && asia_list[2].key == kEnglishKey && asia_list[2].suffix == "CT" && asia_list[2].ps4_id == 10);
    CHECK(TextLanguagesOf(&kJp100)[1].translation == kPortEnglish);

    const auto switch_to_english = [&](const GameBuild& build) {
        LauncherSettings settings(ini);
        NormalizeTextLanguage(settings, offered(build));
        CHECK(settings.Save());
        return LauncherSettings(ini).Get(kTextLanguageKey);
    };
    WriteAll(ini, "text_language=english\n");
    for (const GameBuild* build : {&kJp100, &kJp102, &kJp103, &kCk100, &kCk, &kUs, &kUs101, &kJp100, &kCk}) {
        CHECK(switch_to_english(*build) == "english");
    }
    CHECK(!fs::exists(root / "localization"));
    CHECK(told(kUs, "english") == 1 && told(kUs101, "english") == 1 && told(kCk, "english") == 10);
    for (const GameBuild* japan : {&kJp100, &kJp102, &kJp103}) CHECK(told(*japan, "english") == 0);
    fs::remove_all(root);
}

// An update's regional archive is laid over the base's: its entries outside the language slots win, and the base keeps
// every language slot.
size_t CpkCount(const fs::path& file) {
    CpkArchive archive;
    return archive.Open(file) ? archive.entries().size() : 0;
}

// Japanese NPC Voices on an update: the launcher offers them only when the finished game folder holds the sound table and
// the Japanese voice banks, an update that replaces other entries of DRACRO.CPK keeps them, and the voices do not touch
// the Menu & Subtitles language.
void TestNpcVoiceResources(const fs::path& base) {
    using namespace cpk_test;
    using namespace language_test;
    const fs::path root = base / "npc_voices";
    fs::remove_all(root);
    CHECK(NpcVoiceResourceNames().size() == 10);

    std::vector<Entry> voices;
    for (const std::string_view name : NpcVoiceResourceNames()) {
        voices.push_back({std::string(name), "voice " + std::string(name), false});
    }
    std::vector<Entry> game = voices;
    game.push_back({"stage/Menu.ftx", "menu base", false});
    game.push_back({"snd/narration_gm20000.acb", "narration", false});
    const auto sorted = [](std::vector<Entry> entries) {
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            std::string x = a.path, y = b.path;
            for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : y) c = char(std::tolower(static_cast<unsigned char>(c)));
            const size_t sx = x.rfind('/'), sy = y.rfind('/');
            return std::make_pair(x.substr(0, sx), x.substr(sx + 1)) < std::make_pair(y.substr(0, sy), y.substr(sy + 1));
        });
        return entries;
    };
    const std::string base_archive = BuildCpk(sorted(game), true);
    const fs::path base_app0 = root / "base";
    WriteAll(base_app0 / "DRACRO.CPK", base_archive);
    CHECK(NpcVoiceResourcesPresent(base_app0) && dcp_npc_voice_resources(base_app0.string().c_str()) == 1);
    CHECK(dcp_npc_voice_resources(nullptr) == 0 && !NpcVoiceResourcesPresent(root / "nowhere"));

    // The game's own archive (read only), when a prepared US folder is there.
    if (const fs::path real = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / "v1.03"; fs::exists(real / "DRACRO.CPK")) {
        CHECK(NpcVoiceResourcesPresent(real));
    }

    // One bank missing, or the sound table, refuses; so does an archive that is not one.
    for (const std::string_view missing : NpcVoiceResourceNames()) {
        std::vector<Entry> without;
        for (const Entry& entry : game) {
            if (entry.path != missing) without.push_back(entry);
        }
        const fs::path app0 = root / ("without " + std::string(missing).substr(4));
        WriteAll(app0 / "DRACRO.CPK", BuildCpk(sorted(without), true));
        CHECK(!NpcVoiceResourcesPresent(app0));
    }
    WriteAll(root / "broken" / "DRACRO.CPK", "not an archive");
    CHECK(!NpcVoiceResourcesPresent(root / "broken"));

    // An update with a partial DRACRO.CPK is laid over the base's: the voices stay, and the update's own entries win.
    const fs::path update = root / "update", staged = root / "staged";
    WriteAll(update / "DRACRO.CPK", BuildCpk(sorted({{"stage/Menu.ftx", "menu update", false}, {"stage/New.ftx", "new", false}}), true));
    WriteAll(update / "eboot.bin", "eboot 1.01");
    CHECK(StageUpdate(base_app0, update, staged, &kUs101).ok());
    CHECK(NpcVoiceResourcesPresent(staged));
    CpkArchive composed;
    CHECK(composed.Open(staged / "DRACRO.CPK"));
    const CpkEntry* menu = composed.Find("stage/Menu.ftx");
    CHECK(menu && Bytes(composed.Read(*menu)) == "menu update" && composed.Find("stage/New.ftx") != nullptr);
    CHECK(ReadAll(base_app0 / "DRACRO.CPK") == base_archive && fs::hard_link_count(staged / "DRACRO.CPK") == 1);

    // An update that replaces other sound entries leaves the banks it does not name.
    const fs::path stripped = root / "stripped_update", stripped_staged = root / "stripped_staged";
    std::vector<Entry> replacement = {{"stage/Menu.ftx", "menu update", false}};
    for (const Entry& entry : game) {
        if (std::string_view(entry.path).starts_with("snd/") && !std::string_view(entry.path).starts_with("snd/se_enemy")) replacement.push_back(entry);
    }
    WriteAll(stripped / "DRACRO.CPK", BuildCpk(sorted(replacement), true));
    WriteAll(stripped / "eboot.bin", "eboot 1.01");
    CHECK(StageUpdate(base_app0, stripped, stripped_staged, &kUs101).ok());
    CHECK(NpcVoiceResourcesPresent(stripped_staged));

    // The voices are not a language: the Menu & Subtitles list comes from the regional archive alone.
    WriteAll(staged / "DRACRO_US.CPK", LanguageArchive({Slot("US", kEnglishSheet), Slot("FR", kEnglishSheet), Slot("DE", kEnglishSheet),
                                                       Slot("IT", kEnglishSheet), Slot("ES", kEnglishSheet), Slot("UK", kEnglishSheet)}));
    const std::vector<TextLanguage> with_voices = InstalledTextLanguages(staged, &kUs101);
    fs::remove(staged / "DRACRO.CPK");
    CHECK(!NpcVoiceResourcesPresent(staged));
    CHECK(SameLanguages(with_voices, InstalledTextLanguages(staged, &kUs101)) && with_voices.size() == 7);
}

void TestKeepBaseLanguages(const fs::path& base) {
    using namespace cpk_test;
    using namespace language_test;
    const fs::path root = base / "keep_languages";
    fs::remove_all(root);
    const auto order_key = [](const std::string& path) {
        const size_t slash = path.rfind('/');
        std::string dir = path.substr(0, slash), name = path.substr(slash + 1);
        for (std::string* text : {&dir, &name}) {
            for (char& c : *text) c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        return std::make_pair(dir, name);
    };
    const auto sorted = [&](std::initializer_list<std::vector<Entry>> parts) {
        std::vector<Entry> entries;
        for (const std::vector<Entry>& part : parts) entries.insert(entries.end(), part.begin(), part.end());
        std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) { return order_key(a.path) < order_key(b.path); });
        return entries;
    };
    const auto data_of = [](const CpkArchive& archive, const std::string& path) {
        const CpkEntry* entry = archive.Find(path);
        return entry ? Bytes(archive.Read(*entry)) : std::string("<missing>");
    };
    const auto keys = [](const fs::path& app0, const GameBuild& build) {
        std::string joined;
        for (const TextLanguage& language : InstalledTextLanguages(app0, &build)) {
            if (!language.translation.empty()) continue;
            joined += (joined.empty() ? "" : " ") + std::string(language.key) + "=" + std::to_string(language.ps4_id);
        }
        return joined;
    };
    const std::string western = "english=1 french=2 german=4 italian=5 spanish=3";
    const std::string& us_languages = western; // the native slots: Brazilian Portuguese follows the English one

    std::vector<Entry> us = Slot("US", kEnglishSheet);
    us.push_back({"msgsheet/StaffRollMsg_PS4_US.fms", "staff", false});
    const std::vector<Entry> shared = {{"other/DraCro.otb", "tool", false},
                                       {"other/US_manual_001.ftx", "manual english", false},
                                       {"other/FR_manual_001.ftx", "manual french", false},
                                       {"msgsheet/AbilityList_EU.fms", "eu", false},
                                       {"stage/Interface.ftx", "interface base", false},
                                       {"stage/Menu.ftx", "menu base", false}};
    const std::string base_bytes = BuildCpk(sorted({shared, us, Slot("FR", kEnglishSheet), Slot("DE", kEnglishSheet),
                                                    Slot("IT", kEnglishSheet), Slot("ES", kEnglishSheet), Slot("UK", kEnglishSheet)}),
                                            true);
    std::vector<Entry> russian_us = Slot("US", kRussianSheet), changed_fr = Slot("FR", Sheet("update french"));
    for (std::vector<Entry>* slot : {&russian_us, &changed_fr}) {
        for (Entry& entry : *slot) {
            entry.data = entry.path.find("SystemMsg") == std::string::npos ? "update " + entry.data : entry.data;
        }
    }
    const std::vector<Entry> update_only = {{"other/US_manual_001.ftx", "manual russian", false},
                                            {"stage/Interface.ftx", "interface update", false},
                                            {"stage/NewThing.ftx", "new thing", false},
                                            {"other/Aaa.bin", "first", false}};
    const std::string update_bytes = BuildCpk(sorted({update_only, russian_us, changed_fr}), true);

    const fs::path base_app0 = root / "base", update_app0 = root / "update", merged_app0 = root / "merged";
    WriteAll(base_app0 / "DRACRO_US.CPK", base_bytes);
    WriteAll(update_app0 / "DRACRO_US.CPK", update_bytes);
    const std::string base_hash = Sha256Hex(base_app0 / "DRACRO_US.CPK");

    // The base alone and the update alone: the update's archive is what collapsed the list.
    CHECK(keys(base_app0, kUs) == us_languages && keys(base_app0, kUs101) == western);
    CHECK(keys(update_app0, kUs101) == "english=1 french=2");

    CpkArchive base_archive;
    CHECK(base_archive.Open(base_app0 / "DRACRO_US.CPK"));
    const std::set<std::string> slots = LanguageSlotSuffixes(base_archive, &kUs101);
    CHECK((slots == std::set<std::string>{"DE", "ES", "FR", "IT", "UK", "US"}));
    for (const char* language_file : {"stage/Menu_FR.ftx", "other/DE_manual_001.ftx", "msgsheet/StaffRollMsg_PS4_US.fms"}) {
        CHECK(InLanguageSlot(language_file, slots));
    }
    for (const char* shared_file : {"stage/Interface.ftx", "other/DraCro.otb", "msgsheet/AbilityList_EU.fms", "stage/R_bg32b_00.bsb"}) {
        CHECK(!InLanguageSlot(shared_file, slots));
    }
    const auto keep = [&](std::string_view path) { return InLanguageSlot(path, slots); };

    // The composed archive: the base's language slots whatever the update holds there, the update's other entries.
    const OverlayResult overlaid = OverlayArchive(base_app0 / "DRACRO_US.CPK", update_app0 / "DRACRO_US.CPK", keep,
                                                  merged_app0 / "DRACRO_US.CPK");
    CHECK(overlaid.ok && overlaid.wrote && overlaid.replaced == 1 && overlaid.added == 2 && overlaid.kept == 9);
    CpkArchive merged;
    CHECK(merged.Open(merged_app0 / "DRACRO_US.CPK"));
    CHECK(merged.entries().size() == base_archive.entries().size() + 2);
    for (const CpkEntry& entry : base_archive.entries()) {
        if (InLanguageSlot(entry.path, slots)) {
            CHECK(data_of(merged, entry.path) == data_of(base_archive, entry.path));
        }
    }
    CHECK(data_of(merged, "msgsheet/SystemMsg_US.fms") == kEnglishSheet);  // not the update's Russian
    CHECK(data_of(merged, "msgsheet/SystemMsg_FR.fms") == kEnglishSheet);  // nor its French
    CHECK(data_of(merged, "other/US_manual_001.ftx") == "manual english");
    CHECK(data_of(merged, "stage/Menu_US.ftx") == "menu US" && data_of(merged, "stage/Menu_FR.ftx") == "menu FR");
    CHECK(data_of(merged, "stage/Interface.ftx") == "interface update");    // update wins outside the slots
    CHECK(data_of(merged, "stage/NewThing.ftx") == "new thing" && data_of(merged, "other/Aaa.bin") == "first");
    CHECK(data_of(merged, "stage/Menu.ftx") == "menu base" && data_of(merged, "other/DraCro.otb") == "tool");
    for (size_t i = 0; i < merged.entries().size(); ++i) {
        CHECK(merged.entries()[i].id == i);
        CHECK(i == 0 || order_key(merged.entries()[i - 1].path) < order_key(merged.entries()[i].path));
    }
    CHECK(keys(merged_app0, kUs101) == us_languages && keys(merged_app0, kUs) == us_languages);
    for (const GameBuild* us_build : {&kUs, &kUs101}) {
        const std::vector<TextLanguage> offered = InstalledTextLanguages(merged_app0, us_build);
        CHECK(LanguageNames(offered) == "English, French, German, Italian, Portuguese (Brazil), Russian, Spanish");
        CHECK(offered[0].key == "english" && offered[0].ps4_id == 1 && offered[0].suffix == "US");
        CHECK(std::none_of(offered.begin(), offered.end(), [](const TextLanguage& language) {
            return language.suffix == "UK" || (language.key == "russian" && language.translation != kPortRussian);
        }));
    }
    CHECK(Sha256Hex(base_app0 / "DRACRO_US.CPK") == base_hash);

    // A partial update with only a Russian US slot and one replaced file.
    const std::string partial = BuildCpk(sorted({russian_us, {{"stage/Interface.ftx", "interface partial", false}}}), true);
    WriteAll(update_app0 / "partial" / "DRACRO_US.CPK", partial);
    const OverlayResult one_slot = OverlayArchive(base_app0 / "DRACRO_US.CPK", update_app0 / "partial" / "DRACRO_US.CPK", keep,
                                                  root / "one_slot" / "DRACRO_US.CPK");
    CHECK(one_slot.ok && one_slot.replaced == 1 && one_slot.added == 0 && one_slot.kept == 4);
    CHECK(keys(root / "one_slot", kUs101) == western);
    CpkArchive one_slot_archive;
    CHECK(one_slot_archive.Open(root / "one_slot" / "DRACRO_US.CPK") && one_slot_archive.entries().size() == base_archive.entries().size());
    CHECK(data_of(one_slot_archive, "stage/Interface.ftx") == "interface partial");

    // An update that holds only language slots changes nothing and writes nothing.
    WriteAll(update_app0 / "slots" / "DRACRO_US.CPK", BuildCpk(sorted({russian_us, changed_fr}), true));
    const OverlayResult only_slots = OverlayArchive(base_app0 / "DRACRO_US.CPK", update_app0 / "slots" / "DRACRO_US.CPK", keep,
                                                    root / "only_slots" / "DRACRO_US.CPK");
    CHECK(only_slots.ok && !only_slots.wrote && !fs::exists(root / "only_slots" / "DRACRO_US.CPK"));

    // New entries that outgrow the table's place (the fixture's table has 10 KB, a filler-heavy base leaves little of it)
    // move the table to the end, and the archive still reads.
    std::vector<Entry> filler, many;
    for (int i = 0; i < 100; ++i) {
        filler.push_back({"other/filler_" + std::to_string(1000 + i) + ".bin", "filler", false});
    }
    for (int i = 0; i < 120; ++i) {
        many.push_back({"stage/extra_" + std::to_string(1000 + i) + ".ftx", "extra " + std::to_string(i), false});
    }
    const std::string big_bytes = BuildCpk(sorted({shared, us, Slot("FR", kEnglishSheet), Slot("DE", kEnglishSheet),
                                                   Slot("IT", kEnglishSheet), Slot("ES", kEnglishSheet), Slot("UK", kEnglishSheet), filler}),
                                           true);
    WriteAll(root / "big" / "DRACRO_US.CPK", big_bytes);
    WriteAll(update_app0 / "many" / "DRACRO_US.CPK", BuildCpk(sorted({many, russian_us}), true));
    const OverlayResult grown = OverlayArchive(root / "big" / "DRACRO_US.CPK", update_app0 / "many" / "DRACRO_US.CPK", keep,
                                               root / "grown" / "DRACRO_US.CPK");
    CHECK(grown.ok && grown.added == 120 && grown.kept == 4);
    CpkArchive big_archive, grown_archive;
    CHECK(big_archive.Open(root / "big" / "DRACRO_US.CPK") && big_archive.toc_offset() == 0x800);
    CHECK(grown_archive.Open(root / "grown" / "DRACRO_US.CPK") && grown_archive.toc_offset() > big_bytes.size());
    CHECK(grown_archive.entries().size() == big_archive.entries().size() + 120);
    CHECK(data_of(grown_archive, "stage/extra_1000.ftx") == "extra 0" && data_of(grown_archive, "stage/extra_1119.ftx") == "extra 119");
    CHECK(data_of(grown_archive, "msgsheet/SystemMsg_US.fms") == kEnglishSheet && data_of(grown_archive, "stage/Interface.ftx") == "interface base");
    CHECK(data_of(grown_archive, "other/filler_1050.bin") == "filler");
    CHECK(keys(root / "grown", kUs101) == western);

    // The install step: the staged game gets the composed archive, the installed base is only read.
    const fs::path update_dir = root / "install_update", staged = root / "install_staged";
    const auto write_update = [&](const std::string& archive) {
        WriteAll(update_dir / "DRACRO_US.CPK", archive);
        WriteAll(update_dir / "eboot.bin", "eboot 1.01");
    };
    WriteAll(base_app0 / "eboot.bin", "eboot 1.00");
    write_update(update_bytes);
    CHECK(StageUpdate(base_app0, update_dir, staged, &kUs101).ok());
    CHECK(keys(staged, kUs101) == western && ReadAll(staged / "eboot.bin") == "eboot 1.01");
    CHECK(!fs::exists(update_dir / "DRACRO_US.CPK") && !fs::exists(staged / "DRACRO_US.CPK.compose"));
    CpkArchive staged_archive;
    CHECK(staged_archive.Open(staged / "DRACRO_US.CPK") && data_of(staged_archive, "stage/Interface.ftx") == "interface update");
    CHECK(data_of(staged_archive, "msgsheet/SystemMsg_US.fms") == kEnglishSheet);
    CHECK(Sha256Hex(base_app0 / "DRACRO_US.CPK") == base_hash && fs::hard_link_count(base_app0 / "DRACRO_US.CPK") == 1);
    CHECK(ReadAll(base_app0 / "DRACRO_US.CPK") == base_bytes);

    // Another build's partial update is laid over the base's archive with every entry of its own, language slots too,
    // and the base's other entries stay; a complete one replaces the archive as a whole.
    const fs::path partial_staged = root / "install_partial";
    write_update(update_bytes);
    CHECK(StageUpdate(base_app0, update_dir, partial_staged).ok() && !fs::exists(update_dir / "DRACRO_US.CPK"));
    CpkArchive partial_archive;
    CHECK(partial_archive.Open(partial_staged / "DRACRO_US.CPK"));
    CHECK(partial_archive.entries().size() == base_archive.entries().size() + 2);
    CHECK(data_of(partial_archive, "msgsheet/SystemMsg_US.fms") == kRussianSheet);
    CHECK(data_of(partial_archive, "stage/Interface.ftx") == "interface update");
    CHECK(data_of(partial_archive, "msgsheet/SystemMsg_DE.fms") == kEnglishSheet);
    CHECK(data_of(partial_archive, "stage/Menu.ftx") == "menu base" && data_of(partial_archive, "stage/NewThing.ftx") == "new thing");
    CHECK(fs::hard_link_count(partial_staged / "DRACRO_US.CPK") == 1 && Sha256Hex(base_app0 / "DRACRO_US.CPK") == base_hash);
    std::vector<Entry> every;
    for (const CpkEntry& entry : base_archive.entries()) {
        every.push_back({entry.path, "complete " + data_of(base_archive, entry.path), false});
    }
    const std::string complete_bytes = BuildCpk(sorted({every}), true);
    const fs::path whole = root / "install_whole";
    write_update(complete_bytes);
    CHECK(StageUpdate(base_app0, update_dir, whole).ok() && ReadAll(whole / "DRACRO_US.CPK") == complete_bytes);
    CHECK(ReadAll(base_app0 / "DRACRO_US.CPK") == base_bytes);
    // An archive the base does not have is placed as it is; one the reader cannot open over a readable base fails the
    // step and writes nothing.
    const fs::path placed = root / "install_placed";
    write_update(update_bytes);
    WriteAll(update_dir / "DRACRO_PATCH_JP.CPK", update_bytes);
    CHECK(StageUpdate(base_app0, update_dir, placed).ok() && ReadAll(placed / "DRACRO_PATCH_JP.CPK") == update_bytes);
    WriteAll(base_app0 / "OTHER.CPK", base_bytes);
    for (const std::string& broken : {std::string("not an archive"), update_bytes.substr(0, 0x3000 + 10)}) {
        const fs::path refused = root / "install_refused";
        fs::remove_all(refused);
        write_update(update_bytes);
        WriteAll(update_dir / "OTHER.CPK", broken);
        const InstallResult result = StageUpdate(base_app0, update_dir, refused);
        CHECK(!result.ok() && result.status == InstallStatus::kReadFailed);
        CHECK(ReadAll(base_app0 / "OTHER.CPK") == base_bytes);
    }
    fs::remove(base_app0 / "OTHER.CPK");

    // Asia and Hong Kong 1.02: no special rule, so a partial regional archive is the base's with the update laid over it
    // and the final language set is the base's.
    {
        const std::string traditional = Sheet("New Game Continue Options Quit ##(TOUCH) @#(34)");
        const std::vector<Entry> ct = Slot("CT", traditional), kr = Slot("KR", traditional);
        const std::string ck_base = BuildCpk(sorted({shared, ct, kr}), true);
        const fs::path ck_root = root / "hk", ck_base_app0 = ck_root / "base", ck_update = ck_root / "update";
        fs::remove_all(ck_root);
        WriteAll(ck_base_app0 / "DRACRO_CK.CPK", ck_base);
        WriteAll(ck_base_app0 / "eboot.bin", "eboot 1.00");
        const std::string ck_hash = Sha256Hex(ck_base_app0 / "DRACRO_CK.CPK");
        WriteAll(ck_update / "DRACRO_CK.CPK", BuildCpk(sorted({{{"stage/Interface.ftx", "interface hk update", false}}, {{"stage/NewThing.ftx", "new hk", false}}}), true));
        WriteAll(ck_update / "eboot.bin", "eboot 1.02");
        const std::string hk_keys = "traditional_chinese=10 korean=9";
        CHECK(keys(ck_base_app0, kCk100) == hk_keys);
        const fs::path ck_staged = ck_root / "staged";
        CHECK(StageUpdate(ck_base_app0, ck_update, ck_staged).ok());
        CpkArchive ck_archive;
        CHECK(ck_archive.Open(ck_staged / "DRACRO_CK.CPK") && ck_archive.entries().size() == CpkCount(ck_base_app0 / "DRACRO_CK.CPK") + 1);
        CHECK(data_of(ck_archive, "stage/Interface.ftx") == "interface hk update" && data_of(ck_archive, "stage/NewThing.ftx") == "new hk");
        CHECK(data_of(ck_archive, "stage/Menu_CT.ftx") == "menu CT" && data_of(ck_archive, "stage/Menu_KR.ftx") == "menu KR");
        CHECK(data_of(ck_archive, "stage/Menu.ftx") == "menu base" && data_of(ck_archive, "other/DraCro.otb") == "tool");
        CHECK(keys(ck_staged, kCk) == hk_keys && SameLanguages(InstalledTextLanguages(ck_staged, &kCk), TextLanguagesOf(&kCk)));
        CHECK(Sha256Hex(ck_base_app0 / "DRACRO_CK.CPK") == ck_hash && fs::hard_link_count(ck_base_app0 / "DRACRO_CK.CPK") == 1);
        CHECK(fs::hard_link_count(ck_staged / "DRACRO_CK.CPK") == 1 && !fs::exists(ck_update / "DRACRO_CK.CPK"));
        CHECK(!fs::exists(ck_staged / "DRACRO_CK.CPK.compose"));
    }

    // An update with language slots only leaves the base's archive in the staged game as it is.
    const fs::path slots_only = root / "install_slots";
    write_update(BuildCpk(sorted({russian_us, changed_fr}), true));
    CHECK(StageUpdate(base_app0, update_dir, slots_only, &kUs101).ok() && ReadAll(slots_only / "DRACRO_US.CPK") == base_bytes);

    // A damaged update archive fails the install step and leaves the base alone.
    for (const std::string& broken : {std::string("not an archive"), update_bytes.substr(0, 0x3000 + 10)}) {
        const fs::path failed = root / "install_failed";
        fs::remove_all(failed);
        write_update(broken);
        const InstallResult result = StageUpdate(base_app0, update_dir, failed, &kUs101);
        CHECK(!result.ok() && result.status == InstallStatus::kReadFailed && !result.detail.empty());
        CHECK(Sha256Hex(base_app0 / "DRACRO_US.CPK") == base_hash && !fs::exists(failed / "DRACRO_US.CPK.compose"));
    }

    // A saved language survives the update.
    const fs::path ini = root / "dcp.ini";
    for (const char* kept : {"french", "spanish", "english", "german", "italian"}) {
        WriteAll(ini, std::string("text_language=") + kept + "\n");
        LauncherSettings settings(ini);
        CHECK(!NormalizeTextLanguage(settings, InstalledTextLanguages(staged, &kUs101)));
        CHECK(settings.Get(kTextLanguageKey) == kept);
    }

    // The game's own archive, when a folder holding it is named: read only, the language slots are recognised, and an
    // update composed over it keeps them.
    if (const char* real = std::getenv("DCP_TEST_REAL_ARCHIVE")) {
        const fs::path real_app0 = PathFromUtf8(real), real_cpk = real_app0 / "DRACRO_US.CPK";
        const std::string real_hash = Sha256Hex(real_cpk);
        CpkArchive real_archive;
        CHECK(real_archive.Open(real_cpk));
        const std::set<std::string> real_slots = LanguageSlotSuffixes(real_archive, &kUs101);
        CHECK((real_slots == std::set<std::string>{"DE", "ES", "FR", "IT", "UK", "US"}));
        CHECK(keys(real_app0, kUs) == western && keys(real_app0, kUs101) == western);
        const auto real_keep = [&](std::string_view path) { return InLanguageSlot(path, real_slots); };
        std::vector<Entry> real_update = {{"stage/Interface.ftx", "interface update", false},
                                          {"stage/Menu_US.ftx", "menu us update", false},
                                          {"other/US_manual_001.ftx", "manual update", false},
                                          {"stage/zz_new.ftx", "new", false},
                                          {"other/AAA.bin", "new first", false}};
        for (const bool grow : {false, true}) {
            for (int i = 0; grow && i < 60; ++i) {
                char name[48];
                std::snprintf(name, sizeof(name), "stage/extra_%02d.ftx", i);
                real_update.push_back({name, "extra", false});
            }
            const fs::path out_app0 = root / (grow ? "real_grown" : "real_merged");
            WriteAll(root / "real_update" / "DRACRO_US.CPK", BuildCpk(sorted({real_update}), true));
            const OverlayResult real_result =
                OverlayArchive(real_cpk, root / "real_update" / "DRACRO_US.CPK", real_keep, out_app0 / "DRACRO_US.CPK");
            CHECK(real_result.ok && real_result.wrote && real_result.replaced == 1 && real_result.kept == 2 &&
                  real_result.added == (grow ? 62u : 2u));
            CpkArchive out_archive;
            CHECK(out_archive.Open(out_app0 / "DRACRO_US.CPK") &&
                  out_archive.entries().size() == real_archive.entries().size() + real_result.added);
            CHECK(data_of(out_archive, "stage/Interface.ftx") == "interface update" && data_of(out_archive, "stage/zz_new.ftx") == "new");
            CHECK(data_of(out_archive, "stage/Menu_US.ftx") == data_of(real_archive, "stage/Menu_US.ftx"));
            CHECK(keys(out_app0, kUs101) == western);
            fs::remove_all(out_app0);
        }
        CHECK(Sha256Hex(real_cpk) == real_hash);
    }
    fs::remove_all(root);
}

// The port's translations: message sheets rebuilt byte for byte, their control sequences, and an archive overlay that
// shows a translated record only when the installed English is the record it was written from.
void TestGameTranslation(const fs::path& base) {
    using namespace cpk_test;
    using namespace language_test;
    const auto sheet_of = [](std::initializer_list<std::string> records) {
        std::string out("FMSB\0\0\0\0", 8);
        Le32(out, 0x20);
        out += std::string(8, '\0');
        Le32(out, uint32_t(records.size()));
        out += '\x03';
        out += std::string(7 + records.size() * 8, '\0');
        for (const std::string& record : records) out += record + '\0';
        out.resize((out.size() + 15) / 16 * 16, '\0');
        const uint32_t size = uint32_t(out.size() - 0x20);
        for (int i = 0; i < 4; ++i) out[size_t(4 + i)] = char(size >> (8 * i));
        return out + "FEOC" + std::string(12, '\0');
    };
    const std::string english = sheet_of({"Start Game", "Floor %d", "Options", "@c(10)##(P_INN)@/c", std::string(0x100, 'x')});
    const std::optional<FmsSheet> sheet = ParseFms(Span(english));
    CHECK(sheet && sheet->records.size() == 5 && sheet->records[1] == "Floor %d");
    CHECK(sheet && Bytes(BuildFms(*sheet)) == english);
    std::string damaged = english;
    damaged[0x20] = 1; // the pointer table is zeros on disk
    CHECK(!ParseFms(Span(damaged)) && !ParseFms(Span(english.substr(0, english.size() - 16))) && !ParseFms({}));

    CHECK((ControlTokens("@#(34)Go ##(TAG) %d%% %3d @c(1,2,3)x@/c @@ @C 100$ %.2f %08lx 5%@/c") ==
           std::vector<std::string>{"@#(34)", "##(TAG)", "%d", "%%", "%3d", "@c(1,2,3)", "@/c", "@@", "@C", "$", "%.2f",
                                    "%08lx", "%", "@/c"}));
    CHECK(SameControlTokens("%s won %d", "%s ganhou %d") && !SameControlTokens("%s won %d", "%d: %s"));
    CHECK(SameControlTokens("@c(10)Inn@/c @#(1)", "@#(1) @c(10)Taverna@/c") && !SameControlTokens("@#(1) go", "vá"));
    CHECK(SameControlTokens("About 50% of eggs", "Cerca de 50% dos ovos") && !SameControlTokens("About 50% of", "%d ovos"));
    CHECK(!SameControlTokens("Chance %d%", "Chance %d") && SameControlTokens("Chance %d%", "Chance de %d%"));

    const fs::path app0 = base / "translation_app0";
    fs::remove_all(app0);
    WriteAll(app0 / "DRACRO_US.CPK", LanguageArchive({Slot("US", english), Slot("FR", english)}));
    const std::string before = ReadAll(app0 / "DRACRO_US.CPK");
    const auto digest = [&](const std::string& text) {
        WriteAll(base / "translation_record.txt", text);
        return Sha256Hex(base / "translation_record.txt");
    };
    const std::string json = "{\"sheet\": \"SystemMsg\", \"entries\": [\n"
        "{\"index\": 0, \"source_sha256\": \"" + digest("Start Game") + "\", \"pt_br\": \"Iniciar jogo\"},\n"
        "{\"index\": 1, \"source_sha256\": \"" + digest("Floor %d") + "\", \"pt_br\": \"Andar\"},\n"
        "{\"index\": [2], \"source_sha256\": \"" + digest("Option") + "\", \"pt_br\": \"Opções\"},\n"
        "{\"index\": 3, \"source_sha256\": \"" + digest("@c(10)##(P_INN)@/c") + "\", \"pt_br\": \"@c(10)##(P_INN)@/c\"},\n"
        "{\"index\": 9, \"source_sha256\": \"" + digest("Start Game") + "\", \"pt_br\": \"x\"}]}";
    const auto file_of = [](const char* name, const std::string& text) {
        return TranslationFile{name, reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    };
    const TranslationFile files[] = {file_of("SystemMsg.json", json)};
    const ArchiveTranslation result = TranslateArchive(app0 / "DRACRO_US.CPK", "US", files);
    CHECK(result.error.empty() && result.overlay && result.translated == 2 && result.sheets == 1 && result.refused == 3);
    CHECK((result.problems == std::vector<std::string>{"SystemMsg 1: control sequences differ from the English",
                                                         "SystemMsg 2: the English record changed",
                                                         "SystemMsg 9: no such record"}));
    std::string seen = before;
    if (result.overlay) {
        seen.resize(result.overlay->length, '\0');
        for (const auto& [offset, data] : result.overlay->patches) {
            std::copy(data.begin(), data.end(), seen.begin() + std::ptrdiff_t(offset));
        }
    }
    CHECK(ReadAll(app0 / "DRACRO_US.CPK") == before);
    WriteAll(base / "translation_seen.cpk", seen);
    CpkArchive original, shown;
    CHECK(original.Open(app0 / "DRACRO_US.CPK") && shown.Open(base / "translation_seen.cpk"));
    for (const CpkEntry& entry : original.entries()) {
        const CpkEntry* same = shown.Find(entry.path);
        if (entry.path != "msgsheet/SystemMsg_US.fms") {
            CHECK(same && shown.Read(*same) == original.Read(entry));
            continue;
        }
        const std::optional<FmsSheet> translated = same ? ParseFms(shown.Read(*same)) : std::nullopt;
        CHECK(translated && (translated->records ==
                             std::vector<std::string>{"Iniciar jogo", "Floor %d", "Options", "@c(10)##(P_INN)@/c", std::string(0x100, 'x')}));
    }

    CHECK(!TranslateArchive(app0 / "DRACRO_US.CPK", "US", {}).overlay);
    const std::string broken = "{\"sheet\": 3}";
    const std::string items = "{\"sheet\": \"ItemList\", \"entries\": []}";
    const std::string tutorial = "{\"sheet\": \"Tutorial\", \"entries\": []}";
    const TranslationFile unusable[] = {file_of("Broken.json", broken), file_of("ItemList.json", items),
                                        file_of("Tutorial.json", tutorial)};
    const ArchiveTranslation none = TranslateArchive(app0 / "DRACRO_US.CPK", "US", unusable);
    CHECK(!none.overlay && none.error.empty() && none.refused == 3 && none.translated == 0);
    CHECK((none.problems == std::vector<std::string>{"Broken.json: not a translation file",
                                                       "msgsheet/ItemList_US.fms: not a message sheet the port can rebuild",
                                                       "msgsheet/Tutorial_US.fms: not in the archive"}));
    const ArchiveTranslation missing = TranslateArchive(app0 / "MISSING.CPK", "US", files);
    CHECK(!missing.overlay && missing.error.rfind("MISSING.CPK: ", 0) == 0);

    // The loader's call: only Brazilian Portuguese is a translation; English and Spanish say nothing.
    const fs::path ini = base / "translation_dcp.ini";
    const fs::path image = base / "translation_image";
    WriteAll(image / "image.json", std::string("{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"") +
                                       kUs.eboot_sha256 + "\"\n}");
    const auto translation = [&](const std::string& saved, const fs::path* image_dir, std::string& message) {
        WriteAll(ini, "text_language=" + saved + "\n");
        uint64_t length = 0;
        DcpFilePatch* patches = nullptr;
        size_t count = 0;
        char text[1024];
        const std::string image_text = image_dir ? image_dir->string() : std::string();
        const char* archive = dcp_text_translation(ini.string().c_str(), image_dir ? image_text.c_str() : nullptr,
                                                   app0.string().c_str(), &length, &patches, &count, text, sizeof(text), 0, nullptr);
        message = text;
        localization::SetActiveLanguage("english");
        return archive;
    };
    std::string message;
    CHECK(!translation("english", &image, message) && message.empty());
    CHECK(!translation("spanish", &image, message) && message.empty());
    CHECK(!translation("brazilian_portuguese", nullptr, message) && message.empty());
    const char* archive = translation("brazilian_portuguese", &image, message);
    if (PtBrTranslationFiles().empty()) {
        CHECK(!archive && message == "Portuguese (Brazil): no record translated yet, showing English");
    } else {
        CHECK(message.rfind("Portuguese (Brazil)", 0) == 0);
    }
    const fs::path installed = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / "v1.03";
    if (fs::exists(installed / "DRACRO_US.CPK")) {
        WriteAll(ini, "text_language=brazilian_portuguese\n");
        const std::string image_text = image.string();
        const auto prepare = [&](DcpFilePatch*& patches, size_t& count) {
            uint64_t length = 0;
            char text[1024];
            return dcp_text_translation(ini.string().c_str(), image_text.c_str(), installed.string().c_str(), &length,
                                        &patches, &count, text, sizeof(text), 0, nullptr);
        };
        DcpFilePatch* patches = nullptr;
        size_t count = 0;
        const bool first_ready = prepare(patches, count) != nullptr && count != 0;
        CHECK(first_ready);
        if (first_ready) {
            const uint8_t* data = static_cast<const uint8_t*>(patches[0].data);
            const std::vector<uint8_t> snapshot(data, data + std::min<uint64_t>(patches[0].size, 64));
            DcpFilePatch* again = nullptr;
            size_t again_count = 0;
            CHECK(prepare(again, again_count) != nullptr && again_count == count);
            CHECK(std::equal(snapshot.begin(), snapshot.end(), data));
        }
        localization::SetActiveLanguage("english");
    }
    fs::remove_all(app0);
    fs::remove_all(image);
    fs::remove(ini);
}

// A font pack of a 4096 x 4096 Thin_1dThin BC7 atlas, blank but for filled rectangles in the cells of the given characters.
std::vector<uint8_t> SyntheticFont(std::string_view glyphs) {
    const size_t data_at = 0x1c0, bytes = size_t(1024) * 1024 * 16;
    std::vector<uint8_t> font(data_at + bytes);
    const auto put = [&](size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) font[at + size_t(i)] = uint8_t(value >> (i * 8));
    };
    std::copy_n("FTEX", 4, font.begin());
    put(8, 0x80);
    put(12, 1);
    std::copy_n("FTX0", 4, font.begin() + 0x80);
    put(0x84, uint32_t(bytes + 0x100));
    put(0x88, 0x40);
    std::copy_n("GNF ", 4, font.begin() + 0xc0);
    const size_t descriptor = 0xc0 + 16;
    put(descriptor + 4, 41u << 20);
    put(descriptor + 8, 4095u | 4095u << 14);
    put(descriptor + 12, 13u << 20);
    put(descriptor + 16, 4095u << 13);
    const auto block = [&](int bx, int by) {
        const int x = bx & 7, y = by & 7;
        const int micro = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
        return data_at + (size_t(by / 8) * 128 + size_t(bx / 8)) * 64 * 16 + size_t(micro) * 16;
    };
    uint8_t solid[16] = {};
    solid[0] = 0x40; // mode 6, every channel's endpoints 127
    for (const int bit : {7, 14, 21, 28, 35, 42}) {
        for (int i = 0; i < 7; ++i) solid[(bit + i) >> 3] |= uint8_t(1 << ((bit + i) & 7));
    }
    for (int bit = 49; bit < 63; ++bit) solid[bit >> 3] |= uint8_t(1 << (bit & 7));
    solid[7] |= 0x80 | 0x40; // endpoint alpha 255 on both P bits
    for (const char c : glyphs) {
        const int cell = uint8_t(c) - 0x1f;
        const int left = (cell % 60) * kLatinFontCellPixels, top = (cell / 60) * kLatinFontCellPixels;
        for (int by = top / 4 + 6; by < top / 4 + 14; ++by) {
            for (int bx = left / 4 + 2; bx < left / 4 + 8; ++bx) std::copy_n(solid, 16, font.begin() + std::ptrdiff_t(block(bx, by)));
        }
    }
    return font;
}

void TestLatinGlyphs() {
    CHECK(FoldLatinLetters("A\xC3\xA7\xC3\xA3o \xC3\x89 @#(34) %d") == "Acao E @#(34) %d");
    CHECK(FoldLatinLetters("\xE2\x98\x85 plain") == "\xE2\x98\x85 plain");
    CHECK(SameArguments("@#(36)%s\xE4\xBD\xBF\xE7\x94\xA8", "Usa %s @#(36)") && !SameArguments("%s", "%d") && !SameArguments("@#(53)x", "@#(34)x"));
    CHECK(SameArguments("@c(14)%s##(ABILITY)", "A %s") && !SameArguments("\xE7\x84\xA1\xE5\x8A\xB9", "%d") );

    std::vector<uint8_t> blank = SyntheticFont("");
    std::string why;
    const std::vector<uint8_t> marks = SyntheticFont("`^~.,");
    CHECK(!ComposeLatinFont(blank, marks, &why) && why == "a base glyph is missing");
    CHECK(blank == SyntheticFont(""));
    std::vector<uint8_t> font = SyntheticFont("ACEIOUaceioux");
    const std::vector<uint8_t> original = font;
    CHECK(ComposeLatinFont(font, marks, &why));
    CHECK(font != original);
    for (int i = 0; i < LATIN_GLYPH_COUNT; ++i) {
        std::vector<uint8_t> alpha;
        CHECK(LatinFontCellAlpha(font, LATIN_FIRST_CELL + i, alpha) && *std::max_element(alpha.begin(), alpha.end()) > 200);
    }
    std::vector<uint8_t> occupied = font;
    CHECK(!ComposeLatinFont(occupied, marks, &why) && why == "the atlas rows are not free" && occupied == font);
    std::vector<uint8_t> not_a_font(64);
    CHECK(!ComposeLatinFont(not_a_font, marks, &why) && why == "not a font atlas the port can read");

    // A main font: a descriptor, its pages, then 'a' and every letter's base with their metrics.
    std::vector<uint8_t> image(0x10000);
    const auto u16_at = [&](size_t at, uint16_t value) {
        image[at] = uint8_t(value);
        image[at + 1] = uint8_t(value >> 8);
    };
    const auto descriptor = [&](size_t at, size_t page, size_t chinese_page) {
        const uint8_t header[16] = {0, 0, 0, 0, 0, 0x10, 0, 0x10, 1, 0x44, 0x44, 2, 2, 0x3c, 0x3c, 0};
        std::copy_n(header, 16, image.begin() + std::ptrdiff_t(at));
        const uint64_t first = page, chinese = chinese_page;
        std::memcpy(image.data() + at + 16, &first, 8);
        std::memcpy(image.data() + at + 16 + 0x4f * 8, &chinese, 8);
    };
    descriptor(0x100, 0x2000, 0x3000);
    for (const char base : std::string_view("ACEIOUaceiou")) {
        u16_at(0x2000 + size_t(uint8_t(base)) * 8, uint16_t(base));
        u16_at(0x2000 + size_t(uint8_t(base)) * 8 + 2, 548);
    }
    u16_at(0x3000 + 0x53 * 8, 2175);
    const uint8_t* page = latin_glyphs_locate(image.data(), image.size(), 0x4F53);
    CHECK(page == image.data() + 0x2000);
    CHECK(!latin_glyphs_locate(image.data(), image.size(), 0x4F54));
    std::vector<uint8_t> twice = image;
    std::copy_n(image.begin() + 0x100, 16 + 256 * 8, twice.begin() + 0x1000 - 0x100);
    CHECK(!latin_glyphs_locate(twice.data(), twice.size(), 0x4F53));
    CHECK(latin_glyphs_install(image.data(), image.size(), page));
    for (int i = 0; i < LATIN_GLYPH_COUNT; ++i) {
        const uint8_t* slot = page + size_t(latin_glyphs[i].code) * 8;
        CHECK((slot[0] | slot[1] << 8) == LATIN_FIRST_CELL + i && (slot[2] | slot[3] << 8) == 548);
    }
    CHECK(latin_glyphs_install(image.data(), image.size(), page)); // the Japanese updates' entries point at the free rows
    u16_at(0x2000 + 0xE7 * 8, 100);
    const std::vector<uint8_t> real_glyph = image;
    CHECK(!latin_glyphs_install(image.data(), image.size(), page) && image == real_glyph);
}

// What can be measured of a letter in its cell: the mark above it, split off at the first blank row under it.
struct GlyphShape {
    int top = -1, bottom = -1, left = -1, right = -1;
    int mark_top = -1, mark_bottom = -1, mark_left = -1, mark_right = -1, base_top = -1;
};

GlyphShape ShapeOf(const std::vector<uint8_t>& alpha) {
    constexpr int kSize = kLatinFontCellPixels, kInk = 40;
    const auto inked = [&](int x, int y) { return alpha[size_t(y) * kSize + size_t(x)] > kInk; };
    const auto row_inked = [&](int y) {
        for (int x = 0; x < kSize; ++x) {
            if (inked(x, y)) return true;
        }
        return false;
    };
    GlyphShape shape;
    for (int y = 0; y < kSize; ++y) {
        if (!row_inked(y)) continue;
        if (shape.top < 0) shape.top = y;
        shape.bottom = y;
        for (int x = 0; x < kSize; ++x) {
            if (!inked(x, y)) continue;
            shape.left = shape.left < 0 ? x : std::min(shape.left, x);
            shape.right = std::max(shape.right, x);
        }
    }
    int y = shape.top;
    while (y >= 0 && y < kSize && row_inked(y)) ++y;
    if (shape.top < 0 || y >= shape.bottom) return shape;
    shape.mark_top = shape.top;
    shape.mark_bottom = y - 1;
    while (y < kSize && !row_inked(y)) ++y;
    shape.base_top = y;
    for (int row = shape.mark_top; row <= shape.mark_bottom; ++row) {
        for (int x = 0; x < kSize; ++x) {
            if (!inked(x, row)) continue;
            shape.mark_left = shape.mark_left < 0 ? x : std::min(shape.mark_left, x);
            shape.mark_right = std::max(shape.mark_right, x);
        }
    }
    return shape;
}

// Native USA cells follow Unicode, without the two signs between the letters.
int UsaLatinCell(uint16_t code) {
    return 3481 + (code - 0xC0) - (code > 0xD7 ? 1 : 0) - (code > 0xF7 ? 1 : 0);
}

// The composed letters of a real font, against their base letters and, when it is here, the USA font as a local reference.
void CheckComposedGlyphs(const char* label, std::vector<uint8_t> font, const std::vector<uint8_t>& marks, const std::vector<uint8_t>& usa) {
    const std::vector<uint8_t> original = font;
    std::string why;
    CHECK(ComposeLatinFont(font, marks, &why));
    CHECK(font.size() == original.size());
    const auto shape_at = [](const std::vector<uint8_t>& atlas, int cell) {
        std::vector<uint8_t> alpha;
        CHECK(LatinFontCellAlpha(atlas, cell, alpha));
        return alpha.empty() ? GlyphShape{} : ShapeOf(alpha);
    };
    const std::string_view reported = "\xE1\xE3\xE7\xE9\xEA\xF3\xF5";
    for (int i = 0; i < LATIN_GLYPH_COUNT; ++i) {
        const LatinGlyph& glyph = latin_glyphs[i];
        const bool capital = glyph.base >= 'A' && glyph.base <= 'Z';
        const GlyphShape letter = shape_at(font, LATIN_FIRST_CELL + i);
        const GlyphShape base = shape_at(original, uint8_t(glyph.base) - 0x1f);
        CHECK(letter.left > 0 && letter.right < kLatinFontCellPixels - 1 && letter.top >= 0);
        CHECK(letter.left == base.left || std::abs(letter.left - base.left) <= 12);
        if (glyph.mark == LATIN_MARK_CEDILLA) {
            CHECK(letter.bottom > base.bottom && letter.bottom < kLatinFontCellPixels);
            CHECK(letter.top == (capital ? letter.top : base.top));
        } else {
            CHECK(letter.bottom == base.bottom);
            CHECK(letter.mark_top >= 0 && letter.mark_bottom >= letter.mark_top);
            const int gap = letter.base_top - letter.mark_bottom - 1;
            CHECK(gap >= 1 && gap <= 3);
            CHECK(capital || glyph.base == 'i' || letter.base_top == base.top);
            const double centre = (letter.mark_left + letter.mark_right) / 2.0, middle = (base.left + base.right) / 2.0;
            CHECK(std::abs(centre - middle) <= 3.5);
        }
        if (glyph.code >= 0x100 || reported.find(char(glyph.code)) == std::string_view::npos) continue;
        std::string line = std::string("PT-BR-GLYPH|") + label + "|U+" + std::to_string(glyph.code) + "|baseline " + std::to_string(letter.bottom);
        if (letter.mark_top >= 0) {
            line += ", mark " + std::to_string(letter.mark_bottom - letter.mark_top + 1) + "h, gap " + std::to_string(letter.base_top - letter.mark_bottom - 1);
        }
        if (!usa.empty() && glyph.mark != LATIN_MARK_CEDILLA) {
            // The USA cells sit lower and the executable lifts them: the comparison is of the mark against its letter.
            const GlyphShape native = shape_at(usa, UsaLatinCell(glyph.code));
            const int native_rise = native.base_top - native.mark_top, rise = letter.base_top - letter.mark_top;
            line += ", USA gap " + std::to_string(native.base_top - native.mark_bottom - 1) + ", mark rise difference " + std::to_string(rise - native_rise) + " px";
            CHECK(std::abs(rise - native_rise) <= 3);
        }
        std::printf("%s\n", line.c_str());
    }
}

void TestComposedGlyphGeometry() {
    const fs::path prepared = fs::path(DCP_SOURCE_DIR) / "game" / "prepared";
    const auto entry = [&](const char* folder, const char* archive, const char* path) {
        CpkArchive cpk;
        std::vector<uint8_t> bytes;
        if (cpk.Open(prepared / folder / archive)) {
            if (const CpkEntry* found = cpk.Find(path)) bytes = cpk.Read(*found);
        }
        return bytes;
    };
    const std::vector<uint8_t> usa = entry("v1.03", "DRACRO_US.CPK", "other/fontUTF16.ftx");
    const std::vector<uint8_t> japanese = entry("CUSA07001-1.00", "DRACRO.CPK", "other/fontUTF16.ftx");
    if (!japanese.empty()) CheckComposedGlyphs("Japan", japanese, japanese, usa);
    const std::vector<uint8_t> chinese = entry("CUSA10209-1.00", "DRACRO_CK.CPK", "other/CT_fontUTF16.ftx");
    const std::vector<uint8_t> shared = entry("CUSA10209-1.00", "DRACRO.CPK", "other/fontUTF16.ftx");
    if (!chinese.empty() && !shared.empty()) CheckComposedGlyphs("Asia/Hong Kong", chinese, shared, usa);
}

// The SystemMsg records the character screen uses: four row labels, the entry prompt and the empty-field placeholder.
struct MessageRecord {
    size_t index;
    const char* pt_br;
};
const MessageRecord kCharacterMessageRecords[] = {
    {844, "Mensagem de morte"}, {845, "Mensagem de ressurrei\xC3\xA7\xC3\xA3o"}, {846, "Mensagem de sele\xC3\xA7\xC3\xA3o"},
    {847, "Mensagem de sauda\xC3\xA7\xC3\xA3o"}, {848, "Digite sua mensagem."}, {849, "Voc\xC3\xAA n\xC3\xA3o tem mensagem."},
};

std::set<size_t> CorpusIndices(std::string_view sheet) {
    std::set<size_t> indices;
    for (const TranslationFile& file : PtBrTranslationFiles()) {
        if (file.name != std::string(sheet) + ".json") continue;
        const std::optional<net::Json> json = net::Json::parse(std::string_view(reinterpret_cast<const char*>(file.data), file.size), 8);
        if (!json) continue;
        for (const net::Json& entry : (*json)["entries"].items()) {
            const net::Json& index = entry["index"];
            if (index.is_array()) {
                for (const net::Json& one : index.items()) indices.insert(size_t(one.as_int()));
            } else {
                indices.insert(size_t(index.as_int()));
            }
        }
    }
    return indices;
}

// The picker's records as served: every class and category translated, unlike the placeholder and each other, in the
// right block. Names and the blocks' markers stay the slot's own.
void CheckServedDefaultBone(const ArchiveTranslation& result, const std::vector<std::string>& native, const std::string& placeholder) {
    const std::vector<std::string>& served = result.default_bone;
    CHECK(served.size() == native.size() && served.size() == 2049);
    if (served.size() != native.size() || served.size() != 2049) return;
    const std::set<size_t> corpus = CorpusIndices("DefaultBone");
    CHECK(corpus.size() == 366);
    const auto pt_br = [&](size_t index) { return served[index] != native[index] && !served[index].empty(); };
    for (int character_class = 0; character_class < DEFAULT_MESSAGE_CLASSES; ++character_class) {
        for (int variant = 0; variant < DEFAULT_MESSAGE_VARIANTS; ++variant) {
            for (int category = 0; category < DEFAULT_MESSAGE_CATEGORIES; ++category) {
                const int record = default_message_record(character_class, DefaultMessageCategory(category), variant);
                CHECK(record >= 1641 && record < 2049 && corpus.count(size_t(record)) == 1);
                CHECK(pt_br(size_t(record)) && served[size_t(record)] != placeholder);
                CHECK(served[size_t(record)].size() <= 2 * DEFAULT_MESSAGE_MAX_UNITS);
            }
            const auto apart = [&](DefaultMessageCategory a, DefaultMessageCategory b) {
                const size_t first = size_t(default_message_record(character_class, a, variant));
                const size_t second = size_t(default_message_record(character_class, b, variant));
                CHECK(native[first] == native[second] || served[first] != served[second]);
            };
            apart(DEFAULT_MESSAGE_GREETING, DEFAULT_MESSAGE_SELECTION);
            apart(DEFAULT_MESSAGE_RESURRECTION, DEFAULT_MESSAGE_DEATH);
        }
    }
    for (size_t i = 0; i < 1635; ++i) CHECK(served[i] == native[i]);
    for (size_t i = 1641; i < 2049; ++i) {
        if (!corpus.count(i)) CHECK(served[i] == native[i]);
    }
}

// What the game reads as SystemMsg once the slot is translated: each row keeps its own record and only corpus records change.
void CheckServedSystemMsg(const ArchiveTranslation& result, const std::vector<std::string>& native) {
    const std::vector<uint8_t>* served = nullptr;
    if (result.overlay) {
        for (const auto& patch : result.overlay->patches) {
            uint32_t count = 0;
            if (patch.second.size() > 0x20) std::memcpy(&count, patch.second.data() + 0x14, 4);
            if (patch.second.size() > 0x20 && !std::memcmp(patch.second.data(), "FMSB", 4) && count == native.size()) served = &patch.second;
        }
    }
    CHECK(served != nullptr);
    if (!served) return;
    const std::optional<FmsSheet> sheet = ParseFms(*served);
    CHECK(sheet && sheet->records.size() == native.size());
    if (!sheet) return;
    std::set<std::string> texts;
    for (const MessageRecord& record : kCharacterMessageRecords) {
        CHECK(sheet->records[record.index] == record.pt_br);
        texts.insert(sheet->records[record.index]);
    }
    CHECK(texts.size() == std::size(kCharacterMessageRecords));
    const std::set<size_t> corpus = CorpusIndices("SystemMsg");
    size_t moved = 0;
    for (size_t i = 0; i < native.size(); ++i) {
        if (sheet->records[i] != native[i]) {
            ++moved;
            CHECK(corpus.count(i) == 1);
        }
    }
    CHECK(moved > 0 && moved <= corpus.size());
}

void TestTranslationAcrossSlots(const fs::path& base) {
    using namespace cpk_test;
    const auto sheet_of = [](const std::vector<std::string>& records) {
        std::string body;
        for (const std::string& record : records) body += record + std::string(1, '\0');
        body.resize((0x20 + records.size() * 8 + body.size() + 15) / 16 * 16 - 0x20 - records.size() * 8, '\0');
        std::string out(0x20 + records.size() * 8, '\0');
        out.replace(0, 4, "FMSB");
        const uint32_t tail = uint32_t(records.size() * 8 + body.size()), header = 0x20, count = uint32_t(records.size());
        std::memcpy(out.data() + 4, &tail, 4);
        std::memcpy(out.data() + 8, &header, 4);
        std::memcpy(out.data() + 0x14, &count, 4);
        return out + body + "FEOC" + std::string(12, '\0');
    };
    const auto digest = [&](const std::string& text) {
        WriteAll(base / "slot_record.txt", text);
        return Sha256Hex(base / "slot_record.txt");
    };
    const std::string japanese = sheet_of({"\xE9\x96\x8B\xE5\xA7\x8B", "%d\xE9\x9A\x8E", "\xE7\x84\xA1\xE5\x8A\xB9", "\xE5\xA4\x89"});
    const std::string objects = sheet_of({"\xE5\xAE\x9D"});
    const fs::path app0 = base / "slot_app0";
    fs::remove_all(app0);
    WriteAll(app0 / "DRACRO.CPK", BuildCpk({{"msgsheet/SystemMsg.fms", japanese, false}, {"msgsheet/ObjectParamMsg.fms", objects, false}}, false));
    const std::string before = ReadAll(app0 / "DRACRO.CPK");

    const std::string sheet = "{\"sheet\": \"SystemMsg\", \"entries\": ["
        "{\"index\": 0, \"source_sha256\": \"english\", \"pt_br\": \"A\xC3\xA7\xC3\xA3o\"},"
        "{\"index\": 1, \"source_sha256\": \"english\", \"pt_br\": \"Andar %d\"},"
        "{\"index\": 2, \"source_sha256\": \"english\", \"pt_br\": \"%d de chance\"},"
        "{\"index\": 3, \"source_sha256\": \"english\", \"pt_br\": \"Trocado\"}]}";
    const std::string object_sheet = "{\"sheet\": \"ObjectParam\", \"entries\": [{\"index\": 0, \"source_sha256\": \"x\", \"pt_br\": \"Ba\xC3\xBA\"}]}";
    const std::string map = "{\"source\": \"JP\", \"sheets\": [{\"sheet\": \"SystemMsg\", \"hashes\": [[0, \"" +
                            digest("\xE9\x96\x8B\xE5\xA7\x8B").substr(0, 16) + "\"], [1, \"" + digest("%d\xE9\x9A\x8E").substr(0, 16) +
                            "\"], [2, \"" + digest("\xE7\x84\xA1\xE5\x8A\xB9").substr(0, 16) +
                            "\"], [3, \"0000000000000000\"]]}, {\"sheet\": \"ObjectParam\", \"hashes\": [[0, \"" +
                            digest("\xE5\xAE\x9D").substr(0, 16) + "\"]]}]}";
    const auto file_of = [](const char* name, const std::string& text) {
        return TranslationFile{name, reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    };
    const TranslationFile files[] = {file_of("SystemMsg.json", sheet), file_of("ObjectParam.json", object_sheet)};
    const TranslationFile source = file_of("JP.json", map);

    // The font cannot be completed here: the letters fold, every other record still shows, and one bad record stays native.
    TranslationRecipe recipe{{}, files};
    recipe.source = &source;
    recipe.font_entry = "other/fontUTF16.ftx";
    recipe.mark_archive = app0 / "DRACRO.CPK";
    const ArchiveTranslation folded = TranslateArchive(app0 / "DRACRO.CPK", recipe);
    CHECK(folded.error.empty() && folded.overlay && folded.folded && !folded.latin_glyphs);
    CHECK(folded.corpus == 5 && folded.translated == 3 && folded.sheets == 2 && folded.refused == 2);
    CHECK((folded.problems == std::vector<std::string>{"other/fontUTF16.ftx: no glyph table for the letters in the executable, letters shown without accents",
                                                         "SystemMsg 2: printf or button sequences differ from the source",
                                                         "SystemMsg 3: the source record changed"}));
    std::string seen = before;
    if (folded.overlay) {
        seen.resize(folded.overlay->length, '\0');
        for (const auto& [offset, data] : folded.overlay->patches) std::copy(data.begin(), data.end(), seen.begin() + std::ptrdiff_t(offset));
    }
    CHECK(ReadAll(app0 / "DRACRO.CPK") == before);
    WriteAll(base / "slot_seen.cpk", seen);
    CpkArchive shown;
    CHECK(shown.Open(base / "slot_seen.cpk"));
    const CpkEntry* entry = shown.Find("msgsheet/SystemMsg.fms");
    const std::optional<FmsSheet> translated = entry ? ParseFms(shown.Read(*entry)) : std::nullopt;
    CHECK(translated && (translated->records == std::vector<std::string>{"Acao", "Andar %d", "\xE7\x84\xA1\xE5\x8A\xB9", "\xE5\xA4\x89"}));
    entry = shown.Find("msgsheet/ObjectParamMsg.fms");
    const std::optional<FmsSheet> object = entry ? ParseFms(shown.Read(*entry)) : std::nullopt;
    CHECK(object && object->records == std::vector<std::string>{"Bau"});

    // With the font and the table, the letters stay and the font is served.
    const std::vector<uint8_t> font = SyntheticFont("ACEIOUaceioux`^~.,");
    const std::string font_text(font.begin(), font.end());
    WriteAll(app0 / "DRACRO.CPK", BuildCpk({{"msgsheet/SystemMsg.fms", japanese, false}, {"msgsheet/ObjectParamMsg.fms", objects, false},
                                            {"other/fontUTF16.ftx", font_text, false}}, false));
    recipe.glyph_table = true;
    const ArchiveTranslation composed = TranslateArchive(app0 / "DRACRO.CPK", recipe);
    CHECK(composed.overlay && composed.latin_glyphs && !composed.folded && composed.translated == 3);
    bool font_served = false;
    if (composed.overlay) {
        for (const auto& patch : composed.overlay->patches) font_served = font_served || patch.second.size() == font.size();
    }
    CHECK(font_served);

    const TranslationFile no_map = file_of("JP.json", "{\"sheets\": 3}");
    recipe.source = &no_map;
    CHECK(!TranslateArchive(app0 / "DRACRO.CPK", recipe).error.empty());
    fs::remove_all(app0);
    fs::remove(base / "slot_seen.cpk");
    fs::remove(base / "slot_record.txt");

    // The real data, when it is here: what the launcher would serve on the prepared games.
    struct RealBuild {
        const GameBuild* build;
        const char* folder;
        size_t applied;
    };
    const RealBuild real[] = {{&kUs, "v1.03", 9670}, {&kJp100, "CUSA07001-1.00", 9649}, {&kJp102, "CUSA07001-1.02", 9649},
                              {&kJp103, "CUSA07001-1.03", 9649}, {&kCk100, "CUSA10209-1.00", 9659}};
    for (const RealBuild& item : real) {
        const fs::path game = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / item.folder;
        const TranslationProfile& profile = *item.build->translation;
        if (!fs::exists(game / profile.archive) || PtBrTranslationFiles().empty()) continue;
        const TextLanguage& language = ResolveTextLanguage(item.build, kPortugueseBrazilKey);
        TranslationRecipe actual{language.suffix, PtBrTranslationFiles()};
        if (!profile.source.empty()) actual.source = PtBrSource(profile.source);
        actual.font_entry = profile.font_entry;
        actual.mark_archive = game / "DRACRO.CPK";
        actual.glyph_table = true;
        const ArchiveTranslation result = TranslateArchive(game / profile.archive, actual);
        CHECK(result.error.empty() && result.overlay && result.corpus == 9670 && result.translated == item.applied);
        CHECK(result.latin_glyphs == !profile.font_entry.empty() && !result.folded);
        std::printf("PT-BR %s: %zu of %zu records, %zu kept native\n", item.build->label, result.translated, result.corpus, result.refused);
        CpkArchive slot;
        CHECK(slot.Open(game / profile.archive));
        const std::string system = language.suffix.empty() ? "msgsheet/SystemMsg.fms" : "msgsheet/SystemMsg_" + std::string(language.suffix) + ".fms";
        const CpkEntry* found = slot.Find(system);
        const std::optional<FmsSheet> native = found ? ParseFms(slot.Read(*found)) : std::nullopt;
        CHECK(native.has_value());
        if (native) CheckServedSystemMsg(result, native->records);
        const std::string bone = language.suffix.empty() ? "msgsheet/DefaultBone.fms" : "msgsheet/DefaultBone_" + std::string(language.suffix) + ".fms";
        const CpkEntry* bone_entry = slot.Find(bone);
        const std::optional<FmsSheet> bone_native = bone_entry ? ParseFms(slot.Read(*bone_entry)) : std::nullopt;
        CHECK(bone_native.has_value());
        if (bone_native) CheckServedDefaultBone(result, bone_native->records, native ? (*native).records[849] : std::string());
        if (!bone_native || !actual.source) continue;

        // A character message whose source changed stays native, and its neighbours stay translated.
        const TranslationFile* bone_sheet = nullptr;
        for (const TranslationFile& file : PtBrTranslationFiles()) {
            if (std::string_view(file.name) == "DefaultBone.json") bone_sheet = &file;
        }
        CHECK(bone_sheet != nullptr);
        if (!bone_sheet) continue;
        std::string changed(reinterpret_cast<const char*>(actual.source->data), actual.source->size);
        const size_t record = changed.find("[1642, \"", changed.find("\"DefaultBone\""));
        CHECK(record != std::string::npos);
        if (record == std::string::npos) continue;
        char& digit = changed[record + 8];
        digit = digit == '0' ? '1' : '0';
        const TranslationFile changed_map{"source.json", reinterpret_cast<const uint8_t*>(changed.data()), changed.size()};
        TranslationRecipe one_sheet{language.suffix, std::span(bone_sheet, 1)};
        one_sheet.source = &changed_map;
        const ArchiveTranslation fallback = TranslateArchive(game / profile.archive, one_sheet);
        CHECK(fallback.translated == 365 && fallback.refused == 1);
        CHECK(fallback.default_bone.size() == 2049);
        if (fallback.default_bone.size() == 2049) {
            CHECK(fallback.default_bone[1642] == bone_native->records[1642]);
            CHECK(fallback.default_bone[1643] == result.default_bone[1643]);
        }
    }

    // A hash that stops matching leaves one record native: the row's neighbours keep their translation.
    {
        const std::string native = "{\"source\": \"JP\", \"sheets\": [{\"sheet\": \"SystemMsg\", \"hashes\": [[0, \"" +
                                   std::string(16, '0') + "\"]]}]}";
        const auto file_of = [](const char* name, const std::string& text) {
            return TranslationFile{name, reinterpret_cast<const uint8_t*>(text.data()), text.size()};
        };
        const std::string one_sheet = "{\"sheet\": \"SystemMsg\", \"entries\": [{\"index\": 0, \"source_sha256\": \"x\", \"pt_br\": \"Morte\"}]}";
        const TranslationFile files[] = {file_of("SystemMsg.json", one_sheet)};
        const TranslationFile map = file_of("JP.json", native);
        const fs::path single = base / "message_fallback.cpk";
        std::string records("FMSB");
        records.resize(0x20, '\0');
        const uint32_t count = 1, header = 0x20, tail = 16;
        std::memcpy(records.data() + 4, &tail, 4);
        std::memcpy(records.data() + 8, &header, 4);
        std::memcpy(records.data() + 0x14, &count, 4);
        records += std::string(8, '\0') + "\xE6\xAD\xBB" + std::string(5, '\0') + "FEOC" + std::string(12, '\0');
        WriteAll(single, cpk_test::BuildCpk({{"msgsheet/SystemMsg.fms", records, false}}, false));
        TranslationRecipe recipe{{}, files};
        recipe.source = &map;
        const ArchiveTranslation refused = TranslateArchive(single, recipe);
        CHECK(!refused.overlay && refused.translated == 0 && refused.refused == 1);
        CHECK((refused.problems == std::vector<std::string>{"SystemMsg 0: the source record changed"}));
        fs::remove(single);
    }

    // The loader's calls on a real game: PT-BR gives the slot's overlay and, in Hong Kong, DRACRO.CPK's; Japanese or
    // Traditional Chinese gives neither, so the native art is served untouched.
    const fs::path hk_game = fs::path(DCP_SOURCE_DIR) / "game" / "prepared" / "CUSA10209-1.00";
    if (fs::exists(hk_game / "DRACRO_CK.CPK") && !PtBrTranslationFiles().empty()) {
        const fs::path ini = base / "stage_dcp.ini", image = base / "stage_image";
        WriteAll(image / "image.json", std::string("{\n  \"title_id\": \"CUSA10209\",\n  \"eboot_sha256\": \"") + kCk100.eboot_sha256 + "\"\n}");
        const auto loader_call = [&](const char* saved, uint64_t& length, DcpFilePatch*& patches, size_t& count, std::string& message) {
            WriteAll(ini, std::string("text_language=") + saved + "\n");
            char text[1024];
            int composed = 0;
            const char* archive = dcp_text_translation(ini.string().c_str(), image.string().c_str(), hk_game.string().c_str(), &length, &patches,
                                                       &count, text, sizeof(text), 1, &composed);
            message = text;
            return archive;
        };
        uint64_t length = 0;
        DcpFilePatch* patches = nullptr;
        size_t count = 0;
        std::string message;
        const char* archive = loader_call("brazilian_portuguese", length, patches, count, message);
        CHECK(archive && std::string_view(archive) == "DRACRO_CK.CPK");
        CHECK(message.find("9659 records translated of 9670") != std::string::npos && message.find(", 4 visual replacements") != std::string::npos);
        const char* stage = dcp_text_stage_overlay(&length, &patches, &count);
        CHECK(stage && std::string_view(stage) == "DRACRO.CPK" && count > 0);
        CHECK(!loader_call("traditional_chinese", length, patches, count, message) && message.empty());
        CHECK(!dcp_text_stage_overlay(&length, &patches, &count));
        localization::SetActiveLanguage("english");
        fs::remove_all(image);
        fs::remove(ini);
    }
}

void TestInGameUiText() {
    const auto& rows = localization::PortugueseBrazilUiText().rows;
    const auto has = [&](const char* english) {
        return std::any_of(rows.begin(), rows.end(), [&](const localization::UiTextRow& r) { return r.english == english; });
    };
    for (const char* english : {"OPTIONS", "CHEATS", "F10 Close", "Not saved to the launcher", "Unavailable", "Infinite Health",
                                "Fill Health", "Add 1,000 Gold", "Level Up (+1 Level)", "Movement Speed", "Experience Multiplier",
                                "Display mode", "VSync", "Windowed", "Cheats are disabled while Online Play is enabled",
                                "Cheats are not available for this game build.", "Applies after a restart.", "Cross", "Circle",
                                "Skip the startup logo and go straight to the title screen",
                                "Unlocks all Gallery content while enabled without changing save progress"}) {
        CHECK(has(english));
    }
    const auto conversions = [](const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '%') continue;
            if (i + 1 < text.size() && text[i + 1] == '%') { ++i; continue; }
            size_t end = i + 1;
            while (end < text.size() && !std::isalpha(static_cast<unsigned char>(text[end]))) ++end;
            out += text.substr(i, end - i + 1);
            i = end;
        }
        return out;
    };
    for (const localization::UiTextRow& row : rows) {
        CHECK(conversions(row.english) == conversions(row.text));
        CHECK(std::count(row.english.begin(), row.english.end(), '\n') == std::count(row.text.begin(), row.text.end(), '\n'));
    }
    for (const char* english : {"Online version mismatch", "You", "Friend", "Host", "Incompatible", "Online, in a room",
                                "more friends are listed in the launcher."}) {
        CHECK(has(english));
    }
    for (const char* source : {"gpu/shim/dcp_overlay.cpp", "gpu/shim/dcp_options_menu.cpp"}) {
        CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / source).find("dcp::Tr(") != std::string::npos);
    }
}

// A visual patch is applied only to the exact source it was made from; any other bytes keep the original.
void TestVisualPatchFallback(const fs::path& base) {
    using namespace cpk_test;
    const std::string entry_name = "stage/Interface_US.ftx";
    std::string source(256, 'a'), target = source;
    target.replace(32, 16, std::string(16, 'b'));
    const auto crc = [](const std::string& text) {
        return uint32_t(crc32(0, reinterpret_cast<const Bytef*>(text.data()), uInt(text.size())));
    };
    const auto le32 = [](std::string& out, uint32_t value) {
        for (int i = 0; i < 4; ++i) out += char(value >> (8 * i));
    };
    const auto patch_for = [&](uint32_t before, uint32_t after) {
        std::string raw("DCPX1", 5);
        raw += std::string(3, '\0');
        le32(raw, 1);
        raw += char(entry_name.size());
        raw += char(0);
        raw += entry_name;
        le32(raw, uint32_t(source.size()));
        le32(raw, before);
        le32(raw, after);
        le32(raw, 1);
        le32(raw, 32);
        raw += target.substr(32, 16);
        std::string packed;
        le32(packed, uint32_t(raw.size()));
        uLongf size = compressBound(uLong(raw.size()));
        std::string deflated(size, '\0');
        compress2(reinterpret_cast<Bytef*>(deflated.data()), &size, reinterpret_cast<const Bytef*>(raw.data()), uLong(raw.size()), 9);
        return packed + deflated.substr(0, size);
    };
    const auto run = [&](const std::string& archive_bytes, const std::string& patch) {
        const fs::path file = base / "visual_fallback.cpk";
        WriteAll(file, archive_bytes);
        const TranslationFile files[] = {{"patch.dcpx.z", reinterpret_cast<const uint8_t*>(patch.data()), patch.size()}};
        return TranslateArchive(file, "US", {}, files);
    };
    const std::string good = patch_for(crc(source), crc(target));
    const ArchiveTranslation applied = run(BuildCpk({{entry_name, source, false}}, true), good);
    CHECK(applied.overlay && applied.visuals == 1);
    std::string seen;
    if (applied.overlay) {
        for (const auto& [offset, data] : applied.overlay->patches) {
            if (data.size() == target.size()) seen.assign(data.begin(), data.end());
        }
    }
    CHECK(seen == target);
    const ArchiveTranslation other_source = run(BuildCpk({{entry_name, std::string(256, 'z'), false}}, true), good);
    CHECK(other_source.visuals == 0 && other_source.visual_skipped >= 1);
    const ArchiveTranslation wrong_result = run(BuildCpk({{entry_name, source, false}}, true), patch_for(crc(source), crc(source)));
    CHECK(wrong_result.visuals == 0 && wrong_result.visual_skipped >= 1);
    const ArchiveTranslation missing = run(BuildCpk({{"stage/Other.ftx", source, false}}, true), good);
    CHECK(missing.visuals == 0 && missing.visual_skipped >= 1);
    const std::string garbage = "not a patch";
    CHECK(run(BuildCpk({{entry_name, source, false}}, true), garbage).visual_skipped >= 1);

    // One run list, several accepted sources: each source is held to its own result, an unknown one stays as it is.
    const auto shared_patch = [&](std::vector<std::pair<uint32_t, uint32_t>> states) {
        std::string raw("DCPX2\0\0\0", 8);
        le32(raw, 1);
        raw += char(entry_name.size());
        raw += '\0';
        raw += entry_name;
        le32(raw, uint32_t(source.size()));
        le32(raw, uint32_t(states.size()));
        for (const auto& [before, after] : states) {
            le32(raw, before);
            le32(raw, after);
        }
        le32(raw, 1);
        le32(raw, 32);
        raw += target.substr(32, 16);
        std::string packed;
        le32(packed, uint32_t(raw.size()));
        uLongf size = compressBound(uLong(raw.size()));
        std::string deflated(size, '\0');
        compress2(reinterpret_cast<Bytef*>(deflated.data()), &size, reinterpret_cast<const Bytef*>(raw.data()), uLong(raw.size()), 9);
        return packed + deflated.substr(0, size);
    };
    std::string second_source = source;
    second_source[100] = 'q';
    std::string second_target = target;
    second_target[100] = 'q';
    const std::string both = shared_patch({{crc(source), crc(target)}, {crc(second_source), crc(second_target)}});
    CHECK(run(BuildCpk({{entry_name, source, false}}, true), both).visuals == 1);
    const ArchiveTranslation other_state = run(BuildCpk({{entry_name, second_source, false}}, true), both);
    CHECK(other_state.visuals == 1);
    if (other_state.overlay) {
        std::string served;
        for (const auto& [offset, data] : other_state.overlay->patches) {
            if (data.size() == second_target.size()) served.assign(data.begin(), data.end());
        }
        CHECK(served == second_target);
    }
    const ArchiveTranslation unknown_source = run(BuildCpk({{entry_name, std::string(256, 'z'), false}}, true), both);
    CHECK(unknown_source.visuals == 0 && unknown_source.visual_skipped >= 1);
    CHECK(std::find(unknown_source.visual_problems.begin(), unknown_source.visual_problems.end(),
                    entry_name + ": source fingerprint not recognized") != unknown_source.visual_problems.end());
    CHECK(!ParseBlockPatch(std::span(reinterpret_cast<const uint8_t*>("DCPX2\0\0\0\x01\0\0\0\x09"), 13)));

    // A patch set belongs to one kind of archive: the base stage files or the USA packs, never both.
    for (const TranslationFile& file : PtBrVisualPatches(VisualSet::kBaseStage)) CHECK(std::string_view(file.name).starts_with("base-"));
    for (const TranslationFile& file : PtBrVisualPatches(VisualSet::kEnglishSlot)) CHECK(!std::string_view(file.name).starts_with("base-"));
    CHECK(PtBrVisualPatches(VisualSet::kBaseStage).size() + PtBrVisualPatches(VisualSet::kEnglishSlot).size() == PtBrVisualPatches().size());
    for (const GameBuild& build : SupportedBuilds()) {
        const TranslationProfile& profile = *build.translation;
        const std::string_view title = build.title_id;
        CHECK(profile.visuals == (title == "CUSA07001" || title == "CUSA10209" ? VisualSet::kBaseStage : VisualSet::kEnglishSlot));
        CHECK(profile.visual_archive == std::string_view(title == "CUSA10454" ? "DRACRO_US.CPK" : title == "CUSA10487" ? "DRACRO_EU.CPK" : "DRACRO.CPK"));
        CHECK(profile.archive == std::string_view(title == "CUSA10454"   ? "DRACRO_US.CPK"
                                                  : title == "CUSA10487" ? "DRACRO_EU.CPK"
                                                  : title == "CUSA07001" ? "DRACRO.CPK"
                                                                         : "DRACRO_CK.CPK"));
    }
    fs::remove(base / "visual_fallback.cpk");
}

void TestFileBrowser(const fs::path& base) {
    const fs::path root = base / "browser";
    fs::create_directories(root / "Zeta");
    fs::create_directories(root / "alpha");
    fs::create_directories(root / ".hidden");
    for (const char* name : {"b.pkg", "A.PKG", "notes.txt", ".secret.pkg"}) {
        std::ofstream(root / name) << "data";
    }

    FolderListing listing = ListFolder(root, ".pkg", false);
    CHECK(listing.readable);
    CHECK(listing.entries.size() == 4);
    CHECK(listing.entries[0].directory && listing.entries[0].name == "alpha");  // folders first, without regard to case
    CHECK(listing.entries[1].directory && listing.entries[1].name == "Zeta");
    CHECK(!listing.entries[2].directory && listing.entries[2].name == "A.PKG"); // the extension matches in any case
    CHECK(listing.entries[2].size == 4 && listing.entries[3].name == "b.pkg");
    CHECK(ListFolder(root, "", false).entries.size() == 5);                     // every file with an empty extension
    CHECK(ListFolder(root, ".pkg", true).entries.size() == 6);                  // hidden names on request
    CHECK(!ListFolder(root / "missing", ".pkg", false).readable);
    CHECK(!ListFolder(root / "b.pkg", ".pkg", false).readable);

    fs::create_directories(base / "empty");
    const std::vector<Place> places = {{"Empty", base / "empty"}, {"Packages", root}};
    CHECK(StartingFolder(places, ".pkg") == root);                              // the first place that holds a .pkg
    CHECK(StartingFolder({{"Empty", base / "empty"}}, ".pkg") == base / "empty");
    CHECK(!StartingFolder({}, ".pkg").empty());

    const std::vector<Place> standard = StandardPlaces(root);
    CHECK(!standard.empty() && standard.front().path == root);
    for (const Place& place : StandardPlaces(root / "missing")) {
        CHECK(place.path != root / "missing");                                  // only folders that exist
    }

    CHECK(FormatSize(0) == "0 KB");
    CHECK(FormatSize(512 * 1024) == "512 KB");
    CHECK(FormatSize(5ull << 20) == "5.0 MB");
    CHECK(FormatSize(6ull << 30) == "6.0 GB");
}

// A param.sfo of a build with its identity values, unless one of them is given.
std::string SfoFor(const GameBuild& build, const char* app_ver = nullptr, const char* version = nullptr) {
    return MakeSfo({{"TITLE_ID", build.title_id},
                    {"CONTENT_ID", build.content_id},
                    {"APP_VER", app_ver ? app_ver : FinishedLayer(build).app_ver},
                    {"VERSION", version ? version : FinishedLayer(build).sfo_version}});
}

// A package's param.sfo: the identity of a base package ('gd') or an update ('gp') of a build.
std::string PackageSfo(const GameBuild& build, const char* app_ver, const char* version, const char* category) {
    return MakeSfo({{"TITLE_ID", build.title_id},
                    {"CONTENT_ID", build.content_id},
                    {"APP_VER", app_ver},
                    {"VERSION", version},
                    {"CATEGORY", category}});
}

// Each site of the build is checked before any is written: a damaged site leaves the whole image as it was.
void CheckSites(const GameBuild& build) {
    const auto sites = build.resolution_sites;
    CHECK(sites.size() == 23);
    uint64_t size = 0;
    for (const DcpResolutionSite& site : sites) {
        size = std::max<uint64_t>(size, site.offset + site.opcode_length + 4u);
    }
    std::vector<unsigned char> image(size + 16, 0x90);
    for (const DcpResolutionSite& site : sites) {
        std::memcpy(&image[site.offset], site.opcode, site.opcode_length);
        std::memcpy(&image[site.offset + site.opcode_length], &site.value, 4);
    }
    char error[160];
    std::vector<unsigned char> patched = image;
    CHECK(dcp_apply_resolution(patched.data(), patched.size(), sites.data(), sites.size(), &dcp_resolutions[5], error, sizeof(error)));
    std::vector<unsigned char> damaged = image;
    const DcpResolutionSite& bad = sites[11];
    damaged[bad.offset + bad.opcode_length] ^= 1;
    const std::vector<unsigned char> before = damaged;
    CHECK(!dcp_apply_resolution(damaged.data(), damaged.size(), sites.data(), sites.size(), &dcp_resolutions[5], error, sizeof(error)));
    CHECK(damaged == before);
    char at[32];
    snprintf(at, sizeof(at), "0x%x", unsigned(bad.offset));
    CHECK(std::string(error).find(at) != std::string::npos);
}

// The Japanese build is a base package and then its 1.03 update: a base alone is not the finished game, the update
// must match the base, and every other package is refused.
void TestLayers(const fs::path& base) {
    const fs::path root = base / "layers";
    fs::remove_all(root);

    CHECK(kJp100.layers.size() == 1 && !kJp100.layers[0].update);
    CHECK(kJp102.layers.size() == 2 && !kJp102.layers[0].update && kJp102.layers[1].update);
    CHECK(kJp103.layers.size() == 2 && !kJp103.layers[0].update && kJp103.layers[1].update);
    CHECK(std::string(kJp100.folder) == "CUSA07001-1.00" && kJp100.regional_archive == nullptr && kJp100.extra_files.empty());
    CHECK(std::string(kJp102.folder) == "CUSA07001-1.02" && std::string(kJp102.regional_archive) == "DRACRO_PATCH_JP.CPK");
    CHECK(std::string(kJp103.folder) == "CUSA07001-1.03" && std::string(kJp103.regional_archive) == "DRACRO_PATCH_JP.CPK");
    CHECK(kJp102.extra_files.size() == 1 && std::string(kJp102.extra_files[0]) == "sce_discmap_patch.plt");
    CHECK(kJp103.extra_files.size() == 1 && std::string(kJp103.extra_files[0]) == "sce_discmap_patch.plt");
    CHECK(std::string(kJp100.eboot_sha256) == "e838e5c551f4ea2d13a13246dcd5474a888c56aa8abf5110f1219c0ceb0ae05f");
    CHECK(std::string(kJp102.eboot_sha256) == "e512685aef4fa1e50eafec8c9699b611992356cbae84e9b0147febceb49843fe");
    CHECK(std::string(kJp103.eboot_sha256) == "dccfd331b02d462f6586330664b230a887928ed1b3909f8625b49623630d1a64");
    for (const GameBuild* japan : {&kJp100, &kJp102, &kJp103}) {
        CHECK(japan->cheats && japan->xbox_prompts && japan->true_ultrawide && japan->features_by_signature);
        CHECK(!japan->japanese_npc_voices && CheatsAvailable(japan));
    }
    CheckSites(kJp100);
    CheckSites(kJp102);
    CheckSites(kJp103);
    CheckSites(kCk100);
    CHECK(std::string(kJp100.title_id) == "CUSA07001" && std::string(kJp100.content_id) == "JP0005-CUSA07001_00-DRAGONSCROWN0001");
    CHECK(std::string(FinishedLayer(kJp100).app_ver) == "01.00" && std::string(FinishedLayer(kJp100).sfo_version) == "01.00");
    CHECK(std::string(FinishedLayer(kJp102).app_ver) == "01.02" && std::string(FinishedLayer(kJp103).app_ver) == "01.03");

    // A package names the build whose finished game it is: the base names 1.00, each update names its own version.
    const GameCheck base_package = CheckPackageIdentity(PackageSfo(kJp100, "01.00", "01.00", "gd"));
    CHECK(base_package.build == &kJp100 && base_package.layer == 0);
    const GameCheck identity102 = CheckPackageIdentity(PackageSfo(kJp102, "01.02", "01.00", "gp"));
    CHECK(identity102.build == &kJp102 && identity102.layer == 1);
    const GameCheck identity103 = CheckPackageIdentity(PackageSfo(kJp103, "01.03", "01.00", "gp"));
    CHECK(identity103.build == &kJp103 && identity103.layer == 1);
    CHECK(CheckPackageIdentity(PackageSfo(kJp100, "01.03", "01.00", "gd")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kJp100, "01.01", "01.00", "gp")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kUs, "01.03", "01.00", "gp")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kCk, "01.03", "01.00", "gp")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(MakeSfo({{"TITLE_ID", "CUSA07001"}, {"CONTENT_ID", std::string(kCk.content_id)},
                                        {"APP_VER", "01.00"}, {"VERSION", "01.00"}, {"CATEGORY", "gd"}})).state ==
          GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(MakeSfo({{"TITLE_ID", "CUSA00000"}, {"CONTENT_ID", std::string(kJp103.content_id)},
                                        {"APP_VER", "01.03"}, {"VERSION", "01.00"}, {"CATEGORY", "gp"}})).state ==
          GameFileState::kDifferentVersion);

    // The finished game is identified by its finished layer: each Japanese version names its own build.
    CHECK(CheckSfoIdentity(SfoFor(kJp100)).build == &kJp100);
    CHECK(CheckSfoIdentity(SfoFor(kJp102)).build == &kJp102);
    CHECK(CheckSfoIdentity(SfoFor(kJp103)).build == &kJp103);
    CHECK(CheckSfoIdentity(SfoFor(kJp100, "01.01")).state == GameFileState::kDifferentVersion);
    CHECK(CheckSfoIdentity(SfoFor(kJp103, "01.00")).build == &kJp100);

    // Packages reach the installer as identities: a base starts an install, an update needs its base build, and a
    // Japanese update is never laid over another region's game.
    const fs::path pkg = root / "packages";
    const GameRoots empty{root / "install" / "prepared", root / "install" / "image"};
    WriteAll(pkg / "jp-base.pkg", MakePackage(PackageSfo(kJp100, "01.00", "01.00", "gd")));
    CHECK(InstallGame(pkg / "jp-base.pkg", empty).status == InstallStatus::kUnsupportedPackage);
    WriteAll(pkg / "jp-102.pkg", MakePackage(PackageSfo(kJp102, "01.02", "01.00", "gp")));
    CHECK(InstallGame(pkg / "jp-102.pkg", empty).status == InstallStatus::kUpdateNeedsBase);
    WriteAll(pkg / "jp-103.pkg", MakePackage(PackageSfo(kJp103, "01.03", "01.00", "gp")));
    CHECK(InstallGame(pkg / "jp-103.pkg", empty).status == InstallStatus::kUpdateNeedsBase);
    WriteAll(pkg / "us-update.pkg", MakePackage(PackageSfo(kUs, "01.03", "01.00", "gp")));
    CHECK(InstallGame(pkg / "us-update.pkg", empty).status == InstallStatus::kWrongVersion);
    WriteAll(pkg / "us-101.pkg", MakePackage(PackageSfo(kUs101, "01.01", "01.03", "gp")));
    CHECK(InstallGame(pkg / "us-101.pkg", empty).status == InstallStatus::kUpdateNeedsBase);
    CHECK(BaseBuildOf(kJp102) == &SupportedBuilds()[2] && BaseBuildOf(kJp103) == &SupportedBuilds()[2]);
    CHECK(BaseBuildOf(kJp100) == nullptr && BaseBuildOf(kUs) == nullptr && BaseBuildOf(kCk) == &kCk100);
    CHECK(BaseBuildOf(kUs101) == &kUs);
    WriteAll(pkg / "ck-102.pkg", MakePackage(PackageSfo(kCk, "01.02", "01.00", "gp")));
    CHECK(InstallGame(pkg / "ck-102.pkg", empty).status == InstallStatus::kUpdateNeedsBase);
    CHECK(DescribeInstall(InstallStatus::kUpdateNeedsBase).title == "Base game required");
    CHECK(DescribeInstall(InstallStatus::kUpdateNeedsBase).text.find("CUSA10209 1.00 Asia/Hong Kong") != std::string::npos);
    CHECK(DescribeInstall(InstallStatus::kUpdateNeedsBase).text.find("CUSA10454 1.00 USA") != std::string::npos);

    Fixture installed(root, "installed-1.03", kJp103);
    const GameRoots present{root / "present" / "prepared", root / "present" / "image"};
    fs::create_directories(BuildPaths(present, kJp103).app0.parent_path());
    fs::create_directories(BuildPaths(present, kJp103).image_dir.parent_path());
    fs::copy(installed.paths.app0, BuildPaths(present, kJp103).app0, fs::copy_options::recursive);
    fs::copy(installed.paths.image_dir, BuildPaths(present, kJp103).image_dir, fs::copy_options::recursive);
    CHECK(InstallGame(pkg / "jp-103.pkg", present, {}, installed.builds).status == InstallStatus::kUpToDate);
    InstallHooks reinstall;
    reinstall.replace_active = true;
    CHECK(InstallGame(pkg / "jp-103.pkg", present, reinstall, installed.builds).status != InstallStatus::kUpToDate);

    // Each version's own files: 1.00 is complete without any update file; 1.02 and 1.03 need theirs.
    {
        Fixture base_game(root, "jp100-complete", kJp100);
        CHECK(!fs::exists(base_game.paths.app0 / "DRACRO_PATCH_JP.CPK") && !fs::exists(base_game.paths.app0 / "sce_discmap_patch.plt"));
        CHECK(base_game.State() == GameFileState::kValid);
    }
    for (const GameBuild* build : {&kJp102, &kJp103}) {
        Fixture update(root, "jp-update-complete", *build);
        CHECK(update.State() == GameFileState::kValid);
        fs::remove(update.paths.app0 / "DRACRO_PATCH_JP.CPK");
        CHECK(update.State() == GameFileState::kMissing);
        WriteAll(update.paths.app0 / "DRACRO_PATCH_JP.CPK", "jp archive");
        fs::remove(update.paths.app0 / "sce_discmap_patch.plt");
        CHECK(update.State() == GameFileState::kMissing);
    }
    for (const GameBuild* build : {&kJp100, &kJp102, &kJp103}) {
        Fixture other(root, "jp-other-eboot", *build);
        WriteAll(other.paths.app0 / "eboot.bin", "a different eboot");
        CHECK(other.State() == GameFileState::kDifferentVersion);
    }

    // The base 1.00 with an update laid over it: the merged tree is that update's build, the base is not changed, and
    // a 1.02 folder installed earlier survives the 1.03 update.
    Fixture base_game(root, "base-1.00", kJp100);
    const fs::path update102 = root / "update-1.02";
    WriteAll(update102 / "eboot.bin", "eboot 1.02");
    WriteAll(update102 / "DRACRO_PATCH_JP.CPK", "archive 1.02");
    WriteAll(update102 / "sce_discmap_patch.plt", "discmap 1.02");
    WriteAll(update102 / "sce_sys" / "param.sfo", PackageSfo(kJp102, "01.02", "01.00", "gp"));
    const fs::path update103 = root / "update-1.03";
    WriteAll(update103 / "eboot.bin", "eboot 1.03");
    WriteAll(update103 / "DRACRO_PATCH_JP.CPK", "archive 1.03");
    WriteAll(update103 / "sce_discmap_patch.plt", "discmap 1.03");
    WriteAll(update103 / "sce_sys" / "param.sfo", PackageSfo(kJp103, "01.03", "01.00", "gp"));
    const std::string sha102 = Sha256Hex(update102 / "eboot.bin"), sha103 = Sha256Hex(update103 / "eboot.bin");
    GameBuild jp102 = kJp102, jp103 = kJp103;
    jp102.eboot_sha256 = sha102.c_str();
    jp103.eboot_sha256 = sha103.c_str();
    const std::vector<GameBuild> builds = {base_game.build, jp102, jp103};
    // Overlaying moves the update's files into the staged game, so each staging needs its own copy.
    const auto write_update103 = [&] {
        WriteAll(update103 / "eboot.bin", "eboot 1.03");
        WriteAll(update103 / "DRACRO_PATCH_JP.CPK", "archive 1.03");
        WriteAll(update103 / "sce_discmap_patch.plt", "discmap 1.03");
        WriteAll(update103 / "sce_sys" / "param.sfo", PackageSfo(kJp103, "01.03", "01.00", "gp"));
    };

    const fs::path staged102 = root / "staged-1.02";
    CHECK(StageUpdate(base_game.paths.app0, update102, staged102).ok());
    CHECK(CheckGameTree(staged102, builds).build == &builds[1]);
    CHECK(CheckGameTree(staged102, builds).state == GameFileState::kValid);
    CHECK(ReadAll(staged102 / "eboot.bin") == "eboot 1.02" && ReadAll(staged102 / "DRACRO.CPK") == "data");
    CHECK(!fs::exists(staged102 / "DRACRO_US.CPK"));

    const fs::path staged103 = root / "staged-1.03";
    CHECK(StageUpdate(base_game.paths.app0, update103, staged103).ok());
    CHECK(CheckGameTree(staged103, builds).state == GameFileState::kValid && CheckGameTree(staged103, builds).build == &builds[2]);

    const fs::path installed102 = root / "installed-1.02";
    fs::rename(staged102, installed102);
    const fs::path upgraded103 = root / "upgraded-1.03";
    write_update103();
    CHECK(StageUpdate(base_game.paths.app0, update103, upgraded103).ok());
    CHECK(CheckGameTree(upgraded103, builds).build == &builds[2]);
    CHECK(CheckGameTree(installed102, builds).build == &builds[1] && ReadAll(installed102 / "eboot.bin") == "eboot 1.02");
    CHECK(CheckGameTree(base_game.paths.app0, builds).build == &builds[0] && ReadAll(base_game.paths.app0 / "eboot.bin") == kEboot);

    {
        Fixture ck_base(root, "ck-base-1.00", kCk100);
        const fs::path ck_update = root / "ck-update-1.02";
        WriteAll(ck_update / "sce_sys" / "param.sfo", PackageSfo(kCk, "01.02", "01.00", "gp"));
        GameBuild ck102 = kCk;
        ck102.eboot_sha256 = ck_base.build.eboot_sha256;
        const std::vector<GameBuild> ck_builds = {ck_base.build, ck102};
        CHECK(BaseBuildOf(ck_builds[1], ck_builds) == &ck_builds[0]);
        const std::string base_sfo = ReadAll(ck_base.paths.app0 / "sce_sys" / "param.sfo");
        const fs::path ck_staged = root / "ck-staged-1.02";
        CHECK(StageUpdate(ck_base.paths.app0, ck_update, ck_staged).ok());
        const GameCheck staged = CheckGameTree(ck_staged, ck_builds);
        CHECK(staged.state == GameFileState::kValid && staged.build == &ck_builds[1]);
        const GameCheck untouched = CheckGameTree(ck_base.paths.app0, ck_builds);
        CHECK(untouched.state == GameFileState::kValid && untouched.build == &ck_builds[0]);
        CHECK(ReadAll(ck_base.paths.app0 / "sce_sys" / "param.sfo") == base_sfo);
        CHECK(std::string(ck_builds[0].folder) == "CUSA10209-1.00" && std::string(ck_builds[1].folder) == "CUSA10209-1.02");
        CHECK(std::string(ck_builds[0].eboot_sha256) == ck_builds[1].eboot_sha256);
    }
    {
        Fixture us_base(root, "us-base-1.00", kUs);
        const fs::path us_update = root / "us-update-1.01";
        WriteAll(us_update / "eboot.bin", "eboot 1.01");
        WriteAll(us_update / "DRACRO_US.CPK", "archive 1.01");
        WriteAll(us_update / "sce_discmap.plt", "discmap 1.01");
        WriteAll(us_update / "sce_discmap_patch.plt", "discmap patch 1.01");
        WriteAll(us_update / "sce_sys" / "param.sfo", PackageSfo(kUs101, "01.01", "01.03", "gp"));
        const std::string sha101 = Sha256Hex(us_update / "eboot.bin");
        GameBuild us101 = kUs101;
        us101.eboot_sha256 = sha101.c_str();
        const std::vector<GameBuild> us_builds = {us_base.build, us101};
        CHECK(BaseBuildOf(us_builds[1], us_builds) == &us_builds[0]);
        const std::string base_sfo = ReadAll(us_base.paths.app0 / "sce_sys" / "param.sfo");
        const fs::path us_staged = root / "us-staged-1.01";
        CHECK(StageUpdate(us_base.paths.app0, us_update, us_staged).ok());
        const GameCheck staged = CheckGameTree(us_staged, us_builds);
        CHECK(staged.state == GameFileState::kValid && staged.build == &us_builds[1]);
        CHECK(ReadAll(us_staged / "eboot.bin") == "eboot 1.01" && ReadAll(us_staged / "DRACRO_US.CPK") == "archive 1.01");
        CHECK(fs::exists(us_staged / "sce_discmap_patch.plt")); // the update's discmap patch survives the overlay
        const GameCheck untouched = CheckGameTree(us_base.paths.app0, us_builds);
        CHECK(untouched.state == GameFileState::kValid && untouched.build == &us_builds[0]);
        CHECK(ReadAll(us_base.paths.app0 / "sce_sys" / "param.sfo") == base_sfo);
        CHECK(std::string(us_builds[0].folder) == "v1.03" && std::string(us_builds[1].folder) == "CUSA10454-1.01");
        CHECK(std::string(us_builds[0].eboot_sha256) != us_builds[1].eboot_sha256);
    }
    {
        Fixture merged(root, "jp-merged", kJp103);
        CHECK(merged.State() == GameFileState::kValid);
        CHECK(!fs::exists(merged.paths.app0 / "DRACRO_US.CPK"));
        WriteAll(merged.paths.app0 / "eboot.bin", "base eboot");
        CHECK(merged.State() == GameFileState::kDifferentVersion);
    }
    {
        // The Japanese button atlas is DRACRO.CPK's alone: the update's archive is left as it is, and the base's copy
        // behind the hard link is never written.
        using namespace cpk_test;
        const fs::path base_game = root / "jp-prompts-base", prompts = root / "jp-prompts";
        std::string atlas(8192, '\0');
        for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = char((i * 5 + i / 16) & 255);
        std::string xbox = atlas;
        xbox.replace(0x200, 16, 16, 'X');
        const std::string patch = MakePatch({{"stage/Interface.ftx", {atlas, xbox}}, {"stage/Interface_US.ftx", {atlas, xbox}}});
        const std::string shared = BuildCpk({{"stage/Interface.ftx", atlas, true}, {"msgsheet/SystemMsg.fms", "FMSB", false}}, false);
        const std::string update = BuildCpk({{"msgsheet/Other.fms", "FMSB", false}}, false);
        WriteAll(base_game / "DRACRO.CPK", shared);
        fs::create_directories(prompts);
        std::error_code ec;
        fs::create_hard_link(base_game / "DRACRO.CPK", prompts / "DRACRO.CPK", ec);
        if (ec) {
            fs::copy_file(base_game / "DRACRO.CPK", prompts / "DRACRO.CPK");
        }
        WriteAll(prompts / "DRACRO_PATCH_JP.CPK", update);
        CHECK(PromptArchives(prompts, kJp103) == std::vector<fs::path>{prompts / "DRACRO.CPK"});
        CHECK(PromptArchives(base_game, kJp100) == std::vector<fs::path>{base_game / "DRACRO.CPK"});
        CHECK(XboxPromptsAvailable(prompts, kJp103, Span(patch)) && XboxPromptsAvailable(base_game, kJp100, Span(patch)));
        CHECK(SetPromptStyle(prompts, kJp103, PromptStyle::kXbox, Span(patch)).ok);
        CHECK(InstalledPromptStyle(prompts, kJp103, Span(patch)) == PromptStyle::kXbox);
        CHECK(ReadAll(base_game / "DRACRO.CPK") == shared && ReadAll(prompts / "DRACRO_PATCH_JP.CPK") == update);
        CpkArchive patched;
        CHECK(patched.Open(prompts / "DRACRO.CPK") && Bytes(patched.Read(*patched.Find("stage/Interface.ftx"))) == xbox);
        CHECK(SetPromptStyle(prompts, kJp103, PromptStyle::kPlayStation, {}).ok && ReadAll(prompts / "DRACRO.CPK") == shared);

        WriteAll(prompts / "DRACRO_PATCH_JP.CPK", BuildCpk({{"stage/Interface_JP.ftx", atlas, false}}, false));
        std::string why;
        CHECK(!PromptPatchFits(prompts, kJp103, *ParseBlockPatch(Span(patch)), &why));
        CHECK(why == "stage/Interface_JP.ftx in DRACRO_PATCH_JP.CPK is not covered by the patch");
        CHECK(!XboxPromptsAvailable(prompts, kJp103, Span(patch)));
    }
}

// The CUSA10454 base and its 1.01 update: the update is a second layer, never a base package on its own.
void TestUsBuild() {
    CHECK(std::string(kUs.label) == "CUSA10454 1.00 USA" && std::string(kUs.version) == "1.00");
    CHECK(kUs.layers.size() == 1 && std::string(FinishedLayer(kUs).app_ver) == "01.00" &&
          std::string(FinishedLayer(kUs).sfo_version) == "01.03");
    CHECK(std::string(kUs.eboot_sha256) == "f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a");
    CHECK(kUs.cheats && kUs.xbox_prompts && kUs.resolution_sites.size() == 23 && BaseBuildOf(kUs) == nullptr);
    CHECK(kUs.japanese_npc_voices && SupportsJapaneseNpcVoices(&kUs));
    CHECK(kUs101.japanese_npc_voices && SupportsJapaneseNpcVoices(&kUs101));
    CHECK(!SupportsJapaneseNpcVoices(nullptr));

    CHECK(std::string(kUs101.label) == "CUSA10454 1.01 USA" && std::string(kUs101.version) == "1.01");
    CHECK(std::string(kUs101.folder) == "CUSA10454-1.01" && std::string(kUs101.folder) != kUs.folder);
    CHECK(kUs101.layers.size() == 2 && !kUs101.layers[0].update && kUs101.layers[1].update);
    CHECK(std::string(FinishedLayer(kUs101).app_ver) == "01.01" &&
          std::string(FinishedLayer(kUs101).sfo_version) == "01.03");
    CHECK(std::string(kUs101.eboot_sha256) == "434b3bb43f407990f7b861ed57b8007ba459d63af25b1511ce7489be94c908c0");
    CHECK(std::string(kUs101.eboot_sha256) != kUs.eboot_sha256);
    CHECK(kUs101.cheats && kUs101.xbox_prompts && kUs101.japanese_npc_voices && kUs101.resolution_sites.empty());
    CHECK(kUs101.features_by_signature && !kUs.features_by_signature);
    CHECK(kUs101.resolution_by_signature && !RestrictedToNativeResolution(&kUs101));
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(build.japanese_npc_voices == !NativeJapaneseNpcVoices(&build));
        CHECK(SupportsJapaneseNpcVoices(&build) == build.japanese_npc_voices);
    }
    CHECK(BaseBuildOf(kUs101) == &kUs && BaseBuildOf(kUs) == nullptr);
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(build.cheats && CheatsAvailable(&build) && build.xbox_prompts);
        CHECK(build.features_by_signature == (&build != &kUs));
    }
    CHECK(CheatsAvailable(nullptr));

    CHECK(CheckSfoIdentity(SfoFor(kUs, "01.00", "01.03")).build == &kUs);
    CHECK(CheckPackageIdentity(PackageSfo(kUs, "01.00", "01.03", "gd")).build == &kUs);
    CHECK(CheckSfoIdentity(SfoFor(kUs101)).build == &kUs101);
    CHECK(CheckSfoIdentity(SfoFor(kUs101, "01.00", "01.03")).build == &kUs);
    CHECK(CheckPackageIdentity(PackageSfo(kUs101, "01.00", "01.03", "gd")).build == &kUs);
    const GameCheck us_update = CheckPackageIdentity(PackageSfo(kUs101, "01.01", "01.03", "gp"));
    CHECK(us_update.build == &kUs101 && us_update.layer == 1);
    CHECK(CheckPackageIdentity(PackageSfo(kUs101, "01.01", "01.03", "gd")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kUs, "01.00", "01.03", "gp")).state == GameFileState::kDifferentVersion);
    CHECK(CheckSfoIdentity(SfoFor(kUs, "01.02", "01.03")).state == GameFileState::kDifferentVersion);
    CHECK(SupportedBuildsText().find("CUSA10454 1.00 USA, ") == 0);
    const std::string launcher = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "launcher.cpp");
    CHECK(launcher.find("Tr(\". The USA 1.01, Japanese 1.02 and 1.03 and Asia/Hong Kong 1.02 updates are installed over their \"") != std::string::npos);
}

// CUSA10487 is one complete package: APP_VER 01.00, VERSION 01.03, no update and no patch file.
void TestEuropeBuild(const fs::path& base) {
    CHECK(std::string(kEu.title_id) == "CUSA10487" && std::string(kEu.content_id) == "EP2475-CUSA10487_00-DRAGONSCROWNPRO1");
    CHECK(std::string(kEu.version) == "1.03" && std::string(kEu.folder) == "CUSA10487-1.03");
    CHECK(BuildKey(kEu) == "CUSA10487-1.03" && std::string(kEu.label) == "CUSA10487 1.03 Europe");
    CHECK(std::string(kEu.region) == "Europe" && std::string(kEu.regional_archive) == "DRACRO_EU.CPK");
    CHECK(std::string(kEu.eboot_sha256) == "25c58f473337bc1da768d86e4cd12fe8a15e4af48e0826ac9a857fd9e3c09845");
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "README.md").find("`eboot.bin` 22283899 bytes, SHA-256 `" + std::string(kEu.eboot_sha256)) !=
          std::string::npos);
    CHECK(kEu.extra_files.empty() && kEu.layers.size() == 1 && BaseBuildOf(kEu) == nullptr);
    CHECK(std::string(kEu.layers[0].app_ver) == "01.00" && std::string(kEu.layers[0].sfo_version) == "01.03" && !kEu.layers[0].update);
    CHECK(BuildMetadataText(kEu) == "APP_VER 1.00 \xC2\xB7 VERSION 1.03");
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(&build == &kEu || (std::string(build.folder) != kEu.folder && std::string(build.eboot_sha256) != kEu.eboot_sha256));
    }

    const GameCheck package = CheckPackageIdentity(PackageSfo(kEu, "01.00", "01.03", "gd"));
    CHECK(package.state == GameFileState::kValid && package.build == &kEu && package.layer == 0);
    CHECK(CheckSfoIdentity(SfoFor(kEu)).build == &kEu);
    CHECK(CheckPackageIdentity(PackageSfo(kEu, "01.00", "01.03", "gp")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kEu, "01.00", "01.00", "gd")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kEu, "01.03", "01.03", "gd")).state == GameFileState::kDifferentVersion);
    CHECK(CheckSfoIdentity(SfoFor(kEu, "01.03", "01.03")).state == GameFileState::kDifferentVersion);
    // The USA package has the same APP_VER and VERSION; the title decides.
    CHECK(CheckPackageIdentity(PackageSfo(kUs, "01.00", "01.03", "gd")).build == &kUs);

    const fs::path image = base / "eu-image";
    fs::create_directories(image);
    const auto report = [&](const std::string& sha) {
        WriteAll(image / "image.json", "{\n  \"title_id\": \"CUSA10487\",\n  \"eboot_sha256\": \"" + sha +
                                           "\",\n  \"app_ver\": \"01.00\"\n}");
        return BuildOfImageDir(image.string().c_str());
    };
    CHECK(report(kEu.eboot_sha256) == &kEu);
    CHECK(report("7362f2e73396a188e18b64fb7aa7cd762f87e70ee0e71a13d727d95b0ab20796") == nullptr);
    fs::remove_all(image);

    using namespace cpk_test;
    const fs::path app0 = base / "eu-languages";
    fs::remove_all(app0);
    std::vector<Entry> sheets;
    for (const char* suffix : {"DE", "ES", "FR", "IT", "UK", "US"}) {
        sheets.push_back({std::string("msgsheet/SystemMsg_") + suffix + ".fms", "FMSB", false});
        sheets.push_back({std::string("msgsheet/ItemList_") + suffix + ".fms", "FMSB", false});
    }
    WriteAll(app0 / "DRACRO_EU.CPK", BuildCpk(sheets, true));
    CHECK(LanguageNames(InstalledTextLanguages(app0, &kEu)) ==
          "English, French, German, Italian, Portuguese (Brazil), Russian, Spanish");
    CHECK(ResolveTextLanguage(&kEu, {}).key == "english" && ResolveTextLanguage(&kEu, "french").suffix == "FR");
    CHECK(ResolveTextLanguage(&kEu, kPortugueseBrazilKey).key == kPortugueseBrazilKey);
    CHECK(ResolveTextLanguage(&kEu, kRussianKey).suffix == "US" && ResolveTextLanguage(&kEu, kRussianKey).translation == kPortRussian);
    CHECK(kEu.translation->archive == std::string_view("DRACRO_EU.CPK"));

    // No copy of this archive was ever read: Xbox prompts are offered only while its atlases are the patch's.
    const fs::path prompts = base / "eu-prompts";
    fs::remove_all(prompts);
    std::string atlas(8192, '\0');
    for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = char((i * 3 + i / 32) & 255);
    std::string xbox = atlas, other = atlas;
    xbox.replace(0x400, 16, 16, 'X');
    other.replace(0x400, 16, 16, 'O');
    const std::string patch = MakePatch({{"stage/Interface.ftx", {atlas, xbox}}, {"stage/Interface_UK.ftx", {atlas, xbox}}});
    WriteAll(prompts / "DRACRO.CPK", BuildCpk({{"stage/Interface.ftx", atlas, true}}, false));
    WriteAll(prompts / "DRACRO_EU.CPK", BuildCpk({{"stage/Interface_UK.ftx", atlas, true}}, true));
    CHECK(XboxPromptsAvailable(prompts, kEu, Span(patch)));
    WriteAll(prompts / "DRACRO_EU.CPK", BuildCpk({{"stage/Interface_UK.ftx", other, true}}, true));
    CHECK(!XboxPromptsAvailable(prompts, kEu, Span(patch)));
    fs::remove_all(prompts);
    fs::remove_all(app0);
}

// Prepared CUSA10454 folders made before the build was named 1.00 carry the marker version 1.03.
void TestLegacyMarker(const fs::path& base) {
    const auto write_marker = [](Fixture& f, const char* version) {
        WriteAll(f.paths.app0 / kPreparedMarker, std::string("{\n  \"version\": \"") + version + "\",\n  \"scope\": \"full\"\n}");
    };
    const auto marker_of = [](const Fixture& f) { return ReadAll(f.paths.app0 / kPreparedMarker); };

    Fixture us(base, "legacy-us", kUs);
    write_marker(us, "1.03");
    const fs::file_time_type marker_time = fs::last_write_time(us.paths.app0 / kPreparedMarker) - std::chrono::hours(2);
    fs::last_write_time(us.paths.app0 / kPreparedMarker, marker_time);
    const fs::file_time_type eboot_time = fs::last_write_time(us.paths.app0 / "eboot.bin");
    const GameCheck migrated = CheckGameFiles(us.paths, us.builds);
    CHECK(migrated.state == GameFileState::kValid && migrated.build == &us.builds[0]);
    CHECK(JsonString(marker_of(us), "version") == "1.00" && JsonString(marker_of(us), "scope") == "full");
    CHECK(fs::last_write_time(us.paths.app0 / kPreparedMarker) == marker_time);
    CHECK(fs::last_write_time(us.paths.app0 / "eboot.bin") == eboot_time && us.paths.app0.filename() == "app0");
    CHECK(InstalledGameName(migrated.state, migrated.build) == "PS4 CUSA10454 1.00 USA");
    const std::string after = marker_of(us);
    CHECK(CheckGameFiles(us.paths, us.builds).state == GameFileState::kValid && marker_of(us) == after);

    Fixture current(base, "current-us", kUs);
    const std::string current_marker = marker_of(current);
    CHECK(JsonString(current_marker, "version") == "1.00" && current.State() == GameFileState::kValid &&
          marker_of(current) == current_marker);

    Fixture wrong_hash(base, "legacy-us-hash", kUs);
    write_marker(wrong_hash, "1.03");
    fs::remove(wrong_hash.paths.app0 / "eboot.bin");
    WriteAll(wrong_hash.paths.app0 / "eboot.bin", "another eboot");
    CHECK(wrong_hash.State() == GameFileState::kDifferentVersion && JsonString(marker_of(wrong_hash), "version") == "1.03");

    Fixture update(base, "legacy-us-update", kUs);
    WriteAll(update.paths.app0 / "sce_sys" / "param.sfo", SfoFor(kUs, "01.01", "01.03"));
    write_marker(update, "1.03");
    CHECK(update.State() == GameFileState::kDifferentVersion && JsonString(marker_of(update), "version") == "1.03");

    for (const GameBuild* other : {&kUs101, &kJp100, &kJp102, &kJp103, &kCk}) {
        Fixture f(base, "legacy-other", *other);
        const std::string version = other->version;
        write_marker(f, "1.03");
        const std::string before = marker_of(f);
        CHECK((f.State() == GameFileState::kValid) == (version == "1.03") && marker_of(f) == before);
    }
}

// What the About page names the installed game: each complete build by its own label, and nothing when no supported
// build is complete. The active build is the complete game prepared last.
void TestInstalledGameName(const fs::path& base) {
    const fs::path root = base / "installed-name";
    fs::remove_all(root);
    const GameRoots roots{root / "prepared", root / "image"};

    const GameCheck none = CheckGameFiles(ActiveBuildPaths(roots));
    CHECK(none.state == GameFileState::kMissing && none.build == nullptr);
    CHECK(InstalledGameName(none.state, none.build) == "Not installed");

    const auto named = [](const Fixture& fixture) {
        const GameCheck check = CheckGameFiles(fixture.paths, fixture.builds);
        return InstalledGameName(check.state, check.build);
    };
    Fixture us(root, "us", kUs);
    Fixture jp100(root, "jp100", kJp100);
    Fixture jp102(root, "jp102", kJp102);
    Fixture jp103(root, "jp103", kJp103);
    Fixture ck(root, "ck", kCk);
    CHECK(named(us) == "PS4 CUSA10454 1.00 USA");
    CHECK(named(jp100) == "PS4 CUSA07001 1.00 Japan");
    CHECK(named(jp102) == "PS4 CUSA07001 1.02 Japan");
    CHECK(named(jp103) == "PS4 CUSA07001 1.03 Japan");
    CHECK(named(ck) == "PS4 CUSA10209 1.02 Asia/Hong Kong");
    fs::remove(jp102.paths.app0 / "DRACRO_PATCH_JP.CPK");
    CHECK(named(jp102) == "Not installed");

    const GameRoots active{root / "active" / "prepared", root / "active" / "image"};
    const GamePaths base_paths = BuildPaths(active, kJp100), update_paths = BuildPaths(active, kJp103);
    WriteAll(base_paths.app0 / kPreparedMarker, "{\n  \"version\": \"1.00\",\n  \"scope\": \"full\"\n}");
    CHECK(ActiveBuildPaths(active).app0 == base_paths.app0);
    WriteAll(update_paths.app0 / kPreparedMarker, "{\n  \"version\": \"1.03\",\n  \"scope\": \"full\"\n}");
    fs::last_write_time(base_paths.app0 / kPreparedMarker,
                        fs::last_write_time(update_paths.app0 / kPreparedMarker) - std::chrono::hours(1));
    CHECK(ActiveBuildPaths(active).app0 == update_paths.app0);
}

// Gives an installed fixture its own eboot, so its fingerprint names it among the builds of its title.
void GiveEboot(Fixture& fixture, const char* eboot) {
    WriteAll(fixture.paths.app0 / "eboot.bin", eboot);
    fixture.eboot_sha = Sha256Hex(fixture.paths.app0 / "eboot.bin");
    fixture.build.eboot_sha256 = fixture.eboot_sha.c_str();
    fixture.builds = {fixture.build};
    fixture.Rebuild();
}

// Selecting a package makes the exact build it names the active one, older or newer than the active build. A complete
// prepared folder is activated without copying; a missing one goes through the installation path.
void TestActivation(const fs::path& base) {
    const fs::path root = base / "activation";
    fs::remove_all(root);
    Fixture us(root, "us", kUs), ck(root, "ck", kCk);
    Fixture jp100(root, "jp100", kJp100), jp102(root, "jp102", kJp102), jp103(root, "jp103", kJp103);
    GiveEboot(jp100, "eboot 1.00");
    GiveEboot(jp102, "eboot 1.02");
    GiveEboot(jp103, "eboot 1.03");
    const std::vector<GameBuild> builds = {us.build, jp100.build, jp102.build, jp103.build, ck.build};
    // Indexes: 0 US, 1 Japan 1.00, 2 Japan 1.02, 3 Japan 1.03, 4 CUSA10209. The roots are laid out like the game's
    // folder next to the executable, so ResolvePaths sees the same builds.
    const GameRoots roots{root / "exe" / "game" / "prepared", root / "exe" / "game" / "image"};
    const auto place = [&](const Fixture& fixture) {
        const GamePaths target = BuildPaths(roots, fixture.build);
        fs::create_directories(target.app0.parent_path());
        fs::create_directories(target.image_dir.parent_path());
        const auto copy = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
        fs::copy(fixture.paths.app0, target.app0, copy);
        fs::copy(fixture.paths.image_dir, target.image_dir, copy);
    };
    const auto remove_build = [&](size_t index) {
        fs::remove_all(BuildPaths(roots, builds[index]).app0);
        fs::remove_all(BuildPaths(roots, builds[index]).image_dir);
    };
    for (const Fixture* fixture : {&us, &jp100, &jp102, &jp103, &ck}) {
        place(*fixture);
    }
    CHECK(LastPreparedBuild(GameRoots{root / "none", root / "none-image"}, builds) == nullptr);

    // Makes builds[active] the newest prepared build; the others are older. An activation stamps the current time, which
    // must be newer than the marker it replaces whatever the file system's timestamp granularity.
    const auto make_active = [&](size_t active) {
        const auto now = fs::file_time_type::clock::now();
        for (size_t i = 0; i < builds.size(); ++i) {
            fs::last_write_time(BuildPaths(roots, builds[i]).app0 / kPreparedMarker,
                                now - (i == active ? std::chrono::minutes(10) : std::chrono::hours(1)));
        }
    };
    const auto active = [&] { return CheckGameFiles(ActiveBuildPaths(roots, builds), builds); };

    const fs::path pkg = root / "packages";
    WriteAll(pkg / "jp-100.pkg", MakePackage(PackageSfo(kJp100, "01.00", "01.00", "gd")));
    WriteAll(pkg / "jp-102.pkg", MakePackage(PackageSfo(kJp102, "01.02", "01.00", "gp")));
    WriteAll(pkg / "jp-103.pkg", MakePackage(PackageSfo(kJp103, "01.03", "01.00", "gp")));
    WriteAll(pkg / "us.pkg", MakePackage(PackageSfo(kUs, "01.00", "01.03", "gd")));
    WriteAll(pkg / "ck.pkg", MakePackage(PackageSfo(kCk, "01.02", "01.00", "gp")));
    const auto select = [&](const char* name) { return InstallGame(pkg / name, roots, {}, builds); };
    // Selects a package while builds[from] is active; true when builds[to] is activated with no game data copied.
    const auto switches = [&](size_t from, const char* name, size_t to) {
        make_active(from);
        const InstallResult result = select(name);
        return result.status == InstallStatus::kActivated && result.build == &builds[to] && result.bytes == 0 &&
               result.files == 0 && active().build == &builds[to];
    };

    CHECK(switches(1, "jp-102.pkg", 2));
    CHECK(switches(1, "jp-103.pkg", 3));
    CHECK(switches(2, "jp-100.pkg", 1));
    CHECK(switches(2, "jp-103.pkg", 3));
    CHECK(switches(3, "jp-100.pkg", 1));
    CHECK(switches(3, "jp-102.pkg", 2));

    // Only the exact active build is up to date; its game files and the other builds' are left alone.
    make_active(3);
    const fs::path archive_102 = BuildPaths(roots, builds[2]).app0 / "DRACRO.CPK";
    const fs::file_time_type archive_time = fs::last_write_time(archive_102);
    CHECK(select("jp-103.pkg").status == InstallStatus::kUpToDate && active().build == &builds[3]);
    CHECK(select("jp-102.pkg").status == InstallStatus::kActivated && fs::last_write_time(archive_102) == archive_time);
    make_active(1);
    CHECK(select("jp-100.pkg").status == InstallStatus::kUpToDate && active().build == &builds[1]);

    // A prepared 1.03 that fails the check is not activated; the active 1.00 stays.
    make_active(1);
    const fs::path eboot_103 = BuildPaths(roots, builds[3]).app0 / "eboot.bin";
    WriteAll(eboot_103, "changed");
    CHECK(!select("jp-103.pkg").ok() && active().build == &builds[1]);
    WriteAll(eboot_103, "eboot 1.03");

    // A missing 1.03 goes through the installation path: the base 1.00 is enough, 1.02 is not needed. A missing base
    // is reported as such.
    make_active(1);
    remove_build(2);
    remove_build(3);
    CHECK(select("jp-103.pkg").status == InstallStatus::kUnsupportedPackage && active().build == &builds[1]);
    remove_build(1);
    CHECK(select("jp-103.pkg").status == InstallStatus::kUpdateNeedsBase);
    place(jp100);
    place(jp102);
    place(jp103);

    // The three Japanese builds are prepared side by side; switching between them leaves each one whole.
    make_active(2);
    for (const size_t index : {1, 2, 3}) {
        const GameCheck check = CheckGameFiles(BuildPaths(roots, builds[index]), builds);
        CHECK(check.state == GameFileState::kValid && check.build == &builds[index]);
    }

    // The Play card, About page and loader all follow the selected build.
    CHECK(std::string(active().build->label) == "CUSA07001 1.02 Japan");
    CHECK(InstalledGameName(active().state, active().build) == "PS4 CUSA07001 1.02 Japan");
    const Paths loader = ResolvePaths(PathRequest{}, root / "exe");
    CHECK(loader.app0 == BuildPaths(roots, builds[2]).app0 && loader.image_dir == BuildPaths(roots, builds[2]).image_dir);
    CHECK(switches(3, "jp-100.pkg", 1));
    const Paths loader_100 = ResolvePaths(PathRequest{}, root / "exe");
    CHECK(loader_100.app0 == BuildPaths(roots, builds[1]).app0);

    // The US build keeps its single version; the CUSA10209 1.02 update activates over its own prepared folder.
    CHECK(switches(3, "us.pkg", 0));
    CHECK(select("us.pkg").status == InstallStatus::kUpToDate && active().build == &builds[0]);
    CHECK(switches(0, "ck.pkg", 4));
    CHECK(select("ck.pkg").status == InstallStatus::kUpToDate && active().build == &builds[4]);

    CHECK(std::string(DescribeInstall(InstallStatus::kUpToDate).title) == "Already up to date");
    CHECK(std::string(DescribeInstall(InstallStatus::kActivated).title) == "Game activated");
}

// The supported builds: their identities, archives, flags and the boot image report that names them, and the
// checks that keep one build's files out of another build's folders.
void TestSafeMode(const fs::path& base) {
    using namespace cpk_test;
    CHECK(std::string(kArachnophobiaSafeModeKey) == "arachnophobia_safe_mode");
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == kArachnophobiaSafeModeKey && setting.default_value == "0";
    }
    CHECK(managed);

    {
        const std::string shipped = ReadAll(fs::path(DCP_SOURCE_DIR) / "launcher" / "prompts" / "arachnophobia-spider.dcpx");
        const std::optional<BlockPatch> patch = ParseBlockPatch(Span(shipped));
        CHECK(patch && patch->targets.size() == 1);
        if (patch) {
            CHECK(patch->targets[0].name == kSpiderEntry && patch->targets[0].size == 8389072);
            CHECK(patch->targets[0].runs.size() > 10000);
        }
    }

    std::string iface(20480, '\0'), iface_us(24576, '\0'), spider(16384, '\0');
    for (size_t i = 0; i < iface.size(); ++i) iface[i] = char((i * 7 + i / 64) & 255);
    for (size_t i = 0; i < iface_us.size(); ++i) iface_us[i] = char((i * 13 + i / 32) & 255);
    for (size_t i = 0; i < spider.size(); ++i) spider[i] = char((i * 5 + i / 16) & 255);
    std::string want = iface, want_us = iface_us, want_spider = spider;
    for (size_t off : {0x340u, 0x1000u}) want.replace(off, 16, 16, 'X');
    for (size_t off : {0x380u, 0x2000u}) want_us.replace(off, 16, 16, 'Y');
    for (size_t off : {0x100u, 0x2000u, 0x3ff0u}) want_spider.replace(off, 16, 16, 'W');
    const std::string xbox = MakePatch({{"stage/Interface.ftx", {iface, want}}, {"stage/Interface_US.ftx", {iface_us, want_us}}});
    const std::string safe = MakePatch({{kSpiderEntry, {spider, want_spider}}});

    const fs::path app0 = base / "safe_mode_app0";
    fs::create_directories(app0);
    const std::string original_us = BuildCpk({{"stage/Interface_US.ftx", iface_us, true}}, true);
    const std::string original = BuildCpk({{"stage/Interface.ftx", iface, true}, {kSpiderEntry, spider, true}}, false);
    WriteAll(app0 / "DRACRO_US.CPK", original_us);
    WriteAll(app0 / "DRACRO.CPK", original);
    const auto read = [&](const char* archive, const char* entry) {
        CpkArchive a;
        const CpkEntry* e = a.Open(app0 / archive) ? a.Find(entry) : nullptr;
        return e ? Bytes(a.Read(*e)) : std::string("<missing>");
    };
    const auto set = [&](PromptStyle style, bool on) {
        return SetGamePatches(app0, kUs, style, on, Span(xbox), Span(safe));
    };
    CHECK(!InstalledSafeMode(app0));

    CHECK(set(PromptStyle::kXbox, true).ok);
    CHECK(InstalledSafeMode(app0) && InstalledPromptStyle(app0, kUs, Span(xbox)) == PromptStyle::kXbox);
    CHECK(read("DRACRO.CPK", kSpiderEntry) == want_spider && read("DRACRO.CPK", "stage/Interface.ftx") == want);
    CHECK(read("DRACRO_US.CPK", "stage/Interface_US.ftx") == want_us);
    const std::string both = ReadAll(app0 / "DRACRO.CPK");
    PromptsResult again = set(PromptStyle::kXbox, true);
    CHECK(again.ok && ReadAll(app0 / "DRACRO.CPK") == both);

    CHECK(set(PromptStyle::kXbox, false).ok);
    CHECK(!InstalledSafeMode(app0) && InstalledPromptStyle(app0, kUs, Span(xbox)) == PromptStyle::kXbox);
    CHECK(read("DRACRO.CPK", kSpiderEntry) == spider && read("DRACRO.CPK", "stage/Interface.ftx") == want);

    CHECK(set(PromptStyle::kPlayStation, true).ok);
    CHECK(InstalledSafeMode(app0) && InstalledPromptStyle(app0, kUs, Span(xbox)) == PromptStyle::kPlayStation);
    CHECK(read("DRACRO.CPK", kSpiderEntry) == want_spider && read("DRACRO.CPK", "stage/Interface.ftx") == iface);
    CHECK(ReadAll(app0 / "DRACRO_US.CPK") == original_us);

    CHECK(set(PromptStyle::kXbox, true).ok && InstalledSafeMode(app0) && InstalledPromptStyle(app0, kUs, Span(xbox)) == PromptStyle::kXbox);
    CHECK(set(PromptStyle::kPlayStation, false).ok);
    CHECK(ReadAll(app0 / "DRACRO.CPK") == original && ReadAll(app0 / "DRACRO_US.CPK") == original_us);
    CHECK(!InstalledSafeMode(app0) && !fs::exists(UndoPath(app0 / "DRACRO.CPK")));

    CHECK(!SetGamePatches(app0, kUs, PromptStyle::kXbox, true, Span(xbox), Span(std::string("DCPX1"))).ok);
    CHECK(!InstalledSafeMode(app0) && ReadAll(app0 / "DRACRO.CPK") == original);

    CHECK(SetGamePatches(app0, kJp103, PromptStyle::kPlayStation, true, {}, Span(safe)).ok && InstalledSafeMode(app0));
    CHECK(SetGamePatches(app0, kJp103, PromptStyle::kXbox, true, Span(xbox), Span(safe)).ok && InstalledSafeMode(app0));
    CHECK(read("DRACRO.CPK", "stage/Interface.ftx") == want && read("DRACRO.CPK", kSpiderEntry) == want_spider);
    CHECK(ReadAll(app0 / "DRACRO_US.CPK") == original_us);
    CHECK(SetGamePatches(app0, kJp103, PromptStyle::kPlayStation, false, {}, {}).ok && ReadAll(app0 / "DRACRO.CPK") == original);
}

void TestBuilds(const fs::path& base) {
    CHECK(SupportedBuilds().size() == 8);
    CHECK(std::string(kUs.title_id) == "CUSA10454" && std::string(kCk.title_id) == "CUSA10209");
    CHECK(std::string(kUs.folder) == "v1.03" && std::string(kCk.folder) == "CUSA10209-1.02");
    CHECK(std::string(kCk.content_id) == "HP0177-CUSA10209_00-DRAGONSCROWN0001");
    CHECK(std::string(FinishedLayer(kCk).app_ver) == "01.02" && std::string(FinishedLayer(kCk).sfo_version) == "01.00" && std::string(kCk.version) == "1.02");
    CHECK(std::string(kCk.eboot_sha256) == "927a3d0f73284b52e87e6f22dc4ecbea6350450182a94c1dc20d76907b086ac5");
    CHECK(std::string(kCk.regional_archive) == "DRACRO_CK.CPK" && std::string(kUs.regional_archive) == "DRACRO_US.CPK");
    CHECK(kCk.cheats && kCk.xbox_prompts && kUs.cheats && kUs.xbox_prompts);
    // 1.00 and 1.02 are different executables: 1.00 has verified render resolution sites, 1.02 finds them by signature.
    CHECK(kUs.resolution_sites.size() == 23 && kCk.resolution_sites.empty());
    CHECK(kCk100.resolution_sites.size() == 23);
    CHECK(kCk100.resolution_sites.front().offset == 0x5978a8 && kCk100.resolution_sites.back().offset == 0x598128);
    CHECK(!RestrictedToNativeResolution(&kUs) && !RestrictedToNativeResolution(&kCk100));
    CHECK(!RestrictedToNativeResolution(&kCk) && !RestrictedToNativeResolution(nullptr));

    CHECK(CheckSfoIdentity(SfoFor(kCk)).build == &kCk);
    CHECK(CheckSfoIdentity(SfoFor(kCk100)).build == &kCk100);
    CHECK(std::string(kCk100.label) == "CUSA10209 1.00 Asia/Hong Kong" && std::string(kCk100.folder) == "CUSA10209-1.00");
    CHECK(std::string(FinishedLayer(kCk100).app_ver) == "01.00" && std::string(FinishedLayer(kCk100).sfo_version) == "01.02");
    CHECK(std::string(kCk100.eboot_sha256) == "d37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b");
    CHECK(std::string(kCk100.eboot_sha256) != kCk.eboot_sha256 && std::string(kCk100.regional_archive) == "DRACRO_CK.CPK");
    CHECK(kCk100.cheats && kCk100.xbox_prompts && kCk100.japanese_npc_voices && kCk100.features_by_signature);
    CHECK(std::string(kCk100.folder) != kCk.folder);
    CHECK(kCk100.layers.size() == 1 && BaseBuildOf(kCk) == &kCk100 && BaseBuildOf(kCk100) == nullptr);
    CHECK(kCk.layers.size() == 2 && kCk.layers[1].update);
    const GameCheck ck_update = CheckPackageIdentity(PackageSfo(kCk, "01.02", "01.00", "gp"));
    CHECK(ck_update.build == &kCk && ck_update.layer == 1);
    CHECK(CheckPackageIdentity(PackageSfo(kCk, "01.02", "01.00", "gd")).state == GameFileState::kDifferentVersion);
    CHECK(CheckPackageIdentity(PackageSfo(kCk100, "01.00", "01.02", "gd")).layer == 0);
    CHECK(CheckSfoIdentity(SfoFor(kCk100, "01.02")).build == nullptr);
    CHECK(CheckSfoIdentity(SfoFor(kCk100, nullptr, "01.00")).build == nullptr);
    CHECK(CheckPackageIdentity(PackageSfo(kCk100, "01.00", "01.02", "gd")).build == &kCk100);
    CHECK(CheckSfoIdentity(SfoFor(kUs)).build == &kUs);
    CHECK(CheckSfoIdentity(SfoFor(kCk, "01.01")).state == GameFileState::kDifferentVersion);
    CHECK(CheckSfoIdentity(SfoFor(kCk, nullptr, "01.03")).state == GameFileState::kDifferentVersion);
    CHECK(CheckSfoIdentity(SfoFor(kUs, "01.01")).build == &kUs101); // the USA 1.01 update is now a build
    CHECK(CheckSfoIdentity(SfoFor(kUs, "01.02")).state == GameFileState::kDifferentVersion);
    CHECK(CheckSfoIdentity(MakeSfo({{"TITLE_ID", "CUSA00000"},
                                    {"CONTENT_ID", std::string(kCk.content_id)},
                                    {"APP_VER", std::string(FinishedLayer(kCk).app_ver)},
                                    {"VERSION", std::string(FinishedLayer(kCk).sfo_version)}})).state == GameFileState::kDifferentVersion);

    // The boot image report names its build by title and eboot hash; an unknown hash names no build.
    const fs::path report_dir = base / "build-report";
    const auto build_label = [&](const char* title, const char* sha, const char* app_ver = nullptr) {
        const std::string app = app_ver ? std::string(",\n  \"app_ver\": \"") + app_ver + "\"" : std::string();
        WriteAll(report_dir / "image.json", std::string("{\n  \"title_id\": \"") + title + "\",\n  \"eboot_sha256\": \"" + sha + "\"" + app + "\n}");
        DcpGameBuild game{};
        return dcp_game_build(report_dir.string().c_str(), &game) ? std::string(game.label) : std::string();
    };
    CHECK(build_label("CUSA10209", kCk.eboot_sha256, "01.02") == kCk.label);
    CHECK(build_label("CUSA10209", kCk100.eboot_sha256, "01.00") == kCk100.label);
    CHECK(build_label("CUSA10209", kCk.eboot_sha256) == kCk.label);       // each distinct hash names its own build
    CHECK(build_label("CUSA10209", kCk100.eboot_sha256) == kCk100.label);
    CHECK(build_label("CUSA10209", kCk.eboot_sha256, "01.00").empty());   // and never the other one
    CHECK(build_label("CUSA10209", kCk100.eboot_sha256, "01.02").empty());
    CHECK(build_label("CUSA10209", kCk.eboot_sha256, "01.01").empty());
    CHECK(build_label("CUSA10454", kUs.eboot_sha256) == kUs.label);
    CHECK(build_label("CUSA10454", kUs101.eboot_sha256, "01.01") == kUs101.label);
    CHECK(build_label("CUSA10454", kUs101.eboot_sha256) == kUs101.label);   // each distinct hash names its own build
    CHECK(build_label("CUSA10454", kUs.eboot_sha256, "01.01").empty());     // and never the other one
    CHECK(build_label("CUSA10454", kUs101.eboot_sha256, "01.00").empty());
    CHECK(build_label("CUSA10209", "05dad7c244e76df186eb440818bed6c567aadec1180595a04975fab6d848edf2").empty());
    CHECK(build_label("CUSA10209", kUs.eboot_sha256).empty());
    CHECK(build_label("CUSA10454", kCk.eboot_sha256).empty());
    CHECK(build_label("CUSA07001", kJp100.eboot_sha256) == kJp100.label);
    CHECK(build_label("CUSA07001", kJp102.eboot_sha256) == kJp102.label);
    CHECK(build_label("CUSA07001", kJp103.eboot_sha256) == kJp103.label);
    WriteAll(report_dir / "image.json", "{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"" + std::string(kUs.eboot_sha256) + "\"\n}");
    DcpGameBuild us_game{};
    CHECK(dcp_game_build(report_dir.string().c_str(), &us_game) == 1 && us_game.true_ultrawide == 1);
    WriteAll(report_dir / "image.json", "{\n  \"title_id\": \"CUSA10209\",\n  \"eboot_sha256\": \"" + std::string(kCk.eboot_sha256) + "\",\n  \"app_ver\": \"01.02\"\n}");
    DcpGameBuild ck_game{};
    CHECK(dcp_game_build(report_dir.string().c_str(), &ck_game) == 1);
    CHECK(ck_game.cheats == 1 && ck_game.resolution_site_count == 0 && ck_game.resolution_sites == nullptr);
    CHECK(ck_game.true_ultrawide == 1 && ck_game.features_by_signature == 1);

    {
        Fixture ck(base, "ck-valid", kCk);
        CHECK(ck.State() == GameFileState::kValid);
        CHECK(!fs::exists(ck.paths.app0 / "DRACRO_US.CPK"));
    }
    {
        Fixture ck(base, "ck-no-archive", kCk);
        fs::remove(ck.paths.app0 / "DRACRO_CK.CPK");
        WriteAll(ck.paths.app0 / "DRACRO_US.CPK", "data"); // the US archive does not stand in for it
        CHECK(ck.State() == GameFileState::kMissing);
    }
    {
        Fixture ck(base, "ck-other-eboot", kCk);
        WriteAll(ck.paths.app0 / "eboot.bin", "a different build");
        CHECK(ck.State() == GameFileState::kDifferentVersion);
    }
    {
        Fixture us(base, "us-no-regional", kUs);
        fs::remove(us.paths.app0 / "DRACRO_US.CPK");
        CHECK(us.State() == GameFileState::kMissing);
    }

    {
        Fixture ck_source(base, "ck-install-source", kCk);
        const GameRoots roots{base / "ck-install" / "prepared", base / "ck-install" / "image"};
        WriteAll(BuildPaths(roots, kUs).app0 / kPreparedMarker, "us installed");
        CHECK(InstallGame(ck_source.paths.app0, roots, {}, ck_source.builds).status == InstallStatus::kPrepareFailed);
        CHECK(ReadAll(BuildPaths(roots, kUs).app0 / kPreparedMarker) == "us installed");
        CHECK(!fs::exists(BuildPaths(roots, kCk).app0));
    }

    {
        const GameRoots roots{base / "active" / "prepared", base / "active" / "image"};
        fs::remove_all(base / "active");
        CHECK(ActiveBuildPaths(roots).app0 == BuildPaths(roots, kUs).app0);
        const fs::path us_marker = BuildPaths(roots, kUs).app0 / kPreparedMarker;
        const fs::path ck_marker = BuildPaths(roots, kCk).app0 / kPreparedMarker;
        WriteAll(us_marker, "{\"version\": \"1.03\", \"scope\": \"full\"}");
        WriteAll(ck_marker, "{\"version\": \"1.02\", \"scope\": \"full\"}");
        fs::last_write_time(us_marker, fs::last_write_time(ck_marker) - std::chrono::hours(1));
        CHECK(ActiveBuildPaths(roots).app0 == BuildPaths(roots, kCk).app0);
        CHECK(ActiveBuildPaths(roots).image_dir == BuildPaths(roots, kCk).image_dir);
        CHECK(RootsOf(BuildPaths(roots, kCk)).prepared == roots.prepared);
        fs::last_write_time(us_marker, fs::last_write_time(ck_marker) + std::chrono::hours(1));
        CHECK(ActiveBuildPaths(roots).app0 == BuildPaths(roots, kUs).app0);
    }

    {
        // DRACRO_CK.CPK holds message sheets and no button atlas: only DRACRO.CPK takes the patch.
        using namespace cpk_test;
        const fs::path app0 = base / "ck-prompts";
        fs::remove_all(app0);
        std::string atlas(8192, '\0');
        for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = char((i * 3 + i / 32) & 255);
        std::string xbox = atlas;
        xbox.replace(0x400, 16, 16, 'X');
        const std::string patch = MakePatch({{"stage/Interface.ftx", {atlas, xbox}}, {"stage/Interface_US.ftx", {atlas, xbox}}});
        const std::string regional = BuildCpk({{"msgsheet/SystemMsg_CT.fms", "FMSB", false}, {"msgsheet/SystemMsg_KR.fms", "FMSB", true}}, true);
        const std::string shared = BuildCpk({{"stage/Interface.ftx", atlas, true}}, false);
        WriteAll(app0 / "DRACRO_CK.CPK", regional);
        WriteAll(app0 / "DRACRO.CPK", shared);
        CHECK(PromptArchives(app0, kCk) == std::vector<fs::path>{app0 / "DRACRO.CPK"});
        std::string why;
        CHECK(PromptPatchFits(app0, kCk, *ParseBlockPatch(Span(patch)), &why) && XboxPromptsAvailable(app0, kCk, Span(patch)));
        CHECK(SetPromptStyle(app0, kCk, PromptStyle::kXbox, Span(patch)).ok);
        CHECK(InstalledPromptStyle(app0, kCk, Span(patch)) == PromptStyle::kXbox && ReadAll(app0 / "DRACRO_CK.CPK") == regional);
        CHECK(SetPromptStyle(app0, kCk, PromptStyle::kPlayStation, {}).ok);
        CHECK(ReadAll(app0 / "DRACRO_CK.CPK") == regional && ReadAll(app0 / "DRACRO.CPK") == shared);
    }

    // CUSA10209 1.02 has no verified render resolution sites, so it can never receive the 1.00 table: its table is empty
    // and applying it writes nothing.
    CHECK(kCk.resolution_sites.empty() && kCk.resolution_sites.data() != kCk100.resolution_sites.data());
    {
        std::vector<unsigned char> image(64, 0x90);
        const std::vector<unsigned char> before = image;
        char error[160];
        CHECK(dcp_apply_resolution(image.data(), image.size(), kCk.resolution_sites.data(), kCk.resolution_sites.size(),
                                   &dcp_resolutions[5], error, sizeof(error)));
        CHECK(image == before);
    }
}

// CUSA10209 1.02's render size sites are found by signature rather than fixed offsets, so its render resolution and
// aspect ratio are as effective as CUSA10209 1.00's whole-list ones.
void TestCusa10209Graphics(const fs::path& base) {
    const fs::path root = base / "cusa10209-graphics";
    fs::remove_all(root);
    const fs::path file = root / "dcp.ini";
    const fs::path dir100 = root / "image-1.00", dir102 = root / "image-1.02";
    WriteAll(dir100 / "image.json", std::string("{\n  \"title_id\": \"CUSA10209\",\n  \"eboot_sha256\": \"") +
                                        kCk100.eboot_sha256 + "\",\n  \"app_ver\": \"01.00\"\n}");
    WriteAll(dir102 / "image.json", std::string("{\n  \"title_id\": \"CUSA10209\",\n  \"eboot_sha256\": \"") +
                                        kCk.eboot_sha256 + "\",\n  \"app_ver\": \"01.02\"\n}");
    CHECK(BuildOfImageDir(dir100.string().c_str()) == &kCk100);
    CHECK(BuildOfImageDir(dir102.string().c_str()) == &kCk);

    WriteAll(file, "render_resolution=4K\naspect_ratio=21:9\n");
    CHECK(dcp_render_resolution(file.string().c_str(), dir102.string().c_str()) != 0); // 1.02 keeps them too
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir102.string().c_str()) != 0);
    CHECK(dcp_render_resolution(file.string().c_str(), dir100.string().c_str()) != 0); // 1.00 keeps them
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir100.string().c_str()) != 0);
    CHECK(dcp_render_resolution(file.string().c_str(), nullptr) != 0);                 // the unknown build too
    CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) != 0);

    WriteAll(file, "render_resolution=native\naspect_ratio=16:9\n");
    CHECK(dcp_render_resolution(file.string().c_str(), dir102.string().c_str()) == 0);
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir102.string().c_str()) == 0);
}

// CUSA10454 1.01 has no table of its own: it keeps the whole list and finds its sites by signature in its own image, so it
// never receives the 1.00 table.
void TestUs101Graphics(const fs::path& base) {
    const fs::path root = base / "cusa10454-graphics";
    fs::remove_all(root);
    const fs::path file = root / "dcp.ini";
    const fs::path dir100 = root / "image-1.00", dir101 = root / "image-1.01";
    WriteAll(dir100 / "image.json", std::string("{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"") +
                                        kUs.eboot_sha256 + "\",\n  \"app_ver\": \"01.00\"\n}");
    WriteAll(dir101 / "image.json", std::string("{\n  \"title_id\": \"CUSA10454\",\n  \"eboot_sha256\": \"") +
                                        kUs101.eboot_sha256 + "\",\n  \"app_ver\": \"01.01\"\n}");
    CHECK(BuildOfImageDir(dir100.string().c_str()) == &kUs);
    CHECK(BuildOfImageDir(dir101.string().c_str()) == &kUs101);
    CHECK(!RestrictedToNativeResolution(&kUs101) && !RestrictedToNativeResolution(&kUs));
    CHECK(kUs101.resolution_by_signature && !kUs.resolution_by_signature);
    CHECK(kUs101.resolution_sites.empty() && kUs.resolution_sites.size() == 23);
    CHECK(kUs101.resolution_sites.data() != kUs.resolution_sites.data());

    WriteAll(file, "render_resolution=4K\naspect_ratio=21:9\n");
    CHECK(dcp_render_resolution(file.string().c_str(), dir101.string().c_str()) != 0);
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir101.string().c_str()) != 0);
    CHECK(dcp_render_resolution(file.string().c_str(), dir100.string().c_str()) != 0);
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir100.string().c_str()) != 0);

    // 1.01 can never receive the 1.00 table: its own table is empty, and applying it writes nothing.
    std::vector<unsigned char> image(64, 0x90);
    const std::vector<unsigned char> before = image;
    char error[160];
    CHECK(dcp_apply_resolution(image.data(), image.size(), kUs101.resolution_sites.data(),
                               kUs101.resolution_sites.size(), &dcp_resolutions[5], error, sizeof(error)));
    CHECK(image == before);

    WriteAll(file, "render_resolution=native\naspect_ratio=16:9\n");
    CHECK(dcp_render_resolution(file.string().c_str(), dir101.string().c_str()) == 0);
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir101.string().c_str()) == 0);
}

// The launcher shows the finished package's APP_VER and VERSION under the unchanged short build label.
void TestBuildMetadata() {
    struct Expected {
        const GameBuild* build;
        const char* label;
        const char* metadata;
    };
    const Expected expected[] = {
        {&kUs, "CUSA10454 1.00 USA", "APP_VER 1.00 \xC2\xB7 VERSION 1.03"},
        {&kUs101, "CUSA10454 1.01 USA", "APP_VER 1.01 \xC2\xB7 VERSION 1.03"},
        {&kJp100, "CUSA07001 1.00 Japan", "APP_VER 1.00 \xC2\xB7 VERSION 1.00"},
        {&kJp102, "CUSA07001 1.02 Japan", "APP_VER 1.02 \xC2\xB7 VERSION 1.00"},
        {&kJp103, "CUSA07001 1.03 Japan", "APP_VER 1.03 \xC2\xB7 VERSION 1.00"},
        {&kCk100, "CUSA10209 1.00 Asia/Hong Kong", "APP_VER 1.00 \xC2\xB7 VERSION 1.02"},
        {&kCk, "CUSA10209 1.02 Asia/Hong Kong", "APP_VER 1.02 \xC2\xB7 VERSION 1.00"},
    };
    for (const Expected& item : expected) {
        CHECK(std::string(item.build->label) == item.label);
        CHECK(BuildMetadataText(*item.build) == item.metadata);
        CHECK(std::string(FinishedLayer(*item.build).app_ver).size() == 5);
        CHECK(std::string(FinishedLayer(*item.build).sfo_version).size() == 5);
    }
}

DcpResolutionSignature MakeSignature(std::initializer_list<uint8_t> pattern, std::initializer_list<uint8_t> compare,
                                     int lead, uint8_t opcode, uint32_t value, int height) {
    DcpResolutionSignature signature{};
    signature.length = static_cast<uint8_t>(pattern.size());
    signature.lead = static_cast<uint8_t>(lead);
    signature.opcode_length = 1;
    signature.opcode[0] = opcode;
    signature.value = value;
    signature.height = height;
    std::copy(pattern.begin(), pattern.end(), signature.pattern);
    std::copy(compare.begin(), compare.end(), signature.compare);
    return signature;
}

void Plant(std::vector<unsigned char>& image, size_t at, std::initializer_list<unsigned char> bytes) {
    std::copy(bytes.begin(), bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(at));
}

void TestResolutionProbe() {
    CHECK(DCP_RESOLUTION_COUNT == 6);
    CHECK(DCP_RENDER_SIZE_SIGNATURE_COUNT == 23);
    for (const DcpResolutionSignature& signature : dcp_render_size_signatures) {
        CHECK(signature.length <= DCP_SIGNATURE_MAX);
        CHECK(signature.lead + signature.opcode_length + 4 <= signature.length);
        CHECK(signature.opcode_length == 1 || signature.opcode_length == 2);
        CHECK(std::any_of(signature.compare, signature.compare + signature.length, [](uint8_t c) { return c != 0; }));
        CHECK(signature.value == 1920 || signature.value == 3840 || signature.value == 1080 || signature.value == 2160);
    }

    for (const GameBuild& build : SupportedBuilds()) {
        const bool asia_102 = std::string(build.title_id) == "CUSA10209" && std::string(build.version) == "1.02";
        if (asia_102) {
            CHECK(std::string(build.eboot_sha256) ==
                  "927a3d0f73284b52e87e6f22dc4ecbea6350450182a94c1dc20d76907b086ac5");
            CHECK(build.resolution_by_signature && build.resolution_sites.empty());
            CHECK(!RestrictedToNativeResolution(&build));
        } else if (!build.resolution_sites.empty()) {
            CHECK(build.resolution_sites.size() == 23 && !build.resolution_by_signature);
        } else {
            CHECK((&build == &kUs101 || &build == &kEu) && build.resolution_by_signature);
        }
    }

    const DcpResolutionSignature a =
        MakeSignature({0x11, 0x22, 0xb8, 0x80, 0x07, 0x00, 0x00, 0x33}, {1, 1, 0, 0, 0, 0, 0, 1}, 2, 0xb8, 1920, 0);
    const DcpResolutionSignature b =
        MakeSignature({0x90, 0xb9, 0x38, 0x04, 0x00, 0x00}, {1, 1, 1, 1, 1, 1}, 1, 0xb9, 1080, 1);
    const DcpResolutionSignature c =
        MakeSignature({0xe8, 0x00, 0x00, 0x00, 0x00, 0xba, 0x00, 0x0f, 0x00, 0x00}, {1, 0, 0, 0, 0, 1, 1, 1, 1, 1}, 5,
                      0xba, 3840, 0);
    const DcpResolutionSignature set[] = {a, b, c};

    std::vector<unsigned char> image(1024, 0x90);
    Plant(image, 100, {0x11, 0x22, 0xb8, 0x80, 0x07, 0x00, 0x00, 0x33});
    Plant(image, 300, {0x90, 0xb9, 0x38, 0x04, 0x00, 0x00});
    Plant(image, 600, {0xe8, 0xde, 0xad, 0xbe, 0xef, 0xba, 0x00, 0x0f, 0x00, 0x00});
    const std::vector<unsigned char> untouched = image;

    DcpResolutionSite sites[3];
    DcpResolutionSignatureResult results[3];
    CHECK(dcp_find_resolution_sites(image.data(), image.size(), set, 3, sites, results) == 1);
    CHECK(sites[0].offset == 102 && sites[0].value == 1920 && sites[0].height == 0 && sites[0].opcode_length == 1);
    CHECK(sites[1].offset == 301 && sites[1].value == 1080 && sites[1].height == 1);
    CHECK(sites[2].offset == 605 && sites[2].value == 3840 && sites[2].height == 0);
    CHECK(image == untouched);

    DcpResolutionSite one[1];
    DcpResolutionSignatureResult one_result[1];
    CHECK(dcp_find_resolution_sites(image.data(), image.size(), &a, 1, one, one_result) == 1 && one[0].offset == 102);

    DcpResolutionSite kept[3]{};
    kept[0].offset = 7;
    auto missing = image;
    Plant(missing, 300, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
    CHECK(dcp_find_resolution_sites(missing.data(), missing.size(), set, 3, kept, results) == 0);
    CHECK(results[1].matches == 0 && results[0].valid && results[2].valid);
    CHECK(kept[0].offset == 7);

    auto twice = image;
    Plant(twice, 800, {0x11, 0x22, 0xb8, 0x80, 0x07, 0x00, 0x00, 0x33});
    CHECK(dcp_find_resolution_sites(twice.data(), twice.size(), set, 3, kept, results) == 0);
    CHECK(results[0].matches == 2 && !results[0].valid);
    CHECK(kept[0].offset == 7);

    auto changed = image;
    changed[104] = 0x81;
    CHECK(dcp_find_resolution_sites(changed.data(), changed.size(), set, 3, kept, results) == 0);
    CHECK(results[0].matches == 1 && !results[0].valid);
    CHECK(kept[0].offset == 7);

    const DcpResolutionSignature shifted =
        MakeSignature({0x22, 0xb8, 0x80, 0x07, 0x00, 0x00, 0x33}, {1, 1, 1, 1, 1, 1, 1}, 1, 0xb8, 1920, 0);
    const DcpResolutionSignature overlap[] = {a, shifted};
    CHECK(dcp_find_resolution_sites(image.data(), image.size(), overlap, 2, kept, results) == 0);
    CHECK(results[0].valid && results[1].valid && results[0].offset == results[1].offset);

    const DcpResolutionSignature unanchored = MakeSignature({0x11, 0x22}, {0, 0}, 0, 0xb8, 1920, 0);
    CHECK(dcp_find_resolution_sites(image.data(), image.size(), &unanchored, 1, one, one_result) == 0);
    CHECK(one_result[0].matches == 0);

    const std::vector<unsigned char> tiny(4, 0x11);
    CHECK(dcp_find_resolution_sites(tiny.data(), tiny.size(), &a, 1, one, one_result) == 0);
    CHECK(one_result[0].matches == 0);
    CHECK(image == untouched);
}

void TestLauncherLocalization() {
    using namespace localization;
    for (const char* file : {"launcher/localization/ui_text.h", "launcher/localization/ui_text.cpp",
                             "launcher/localization/pt_br_ui_text.cpp"}) {
        const std::string bytes = ReadAll(fs::path(DCP_SOURCE_DIR) / file);
        CHECK(!bytes.empty());
        CHECK(ValidUtf8Text(bytes));
        CHECK(bytes.find("\xEF\xBF\xBD") == std::string::npos);
    }
    const UiTextTable& table = PortugueseBrazilUiText();
    CHECK(table.errors.empty());
    std::set<std::string> english;
    for (const UiTextRow& row : table.rows) {
        CHECK(!row.english.empty());
        CHECK(!row.text.empty());
        CHECK(ValidUtf8Text(row.english));
        CHECK(ValidUtf8Text(row.text));
        CHECK(english.insert(row.english).second);
    }

    SetActiveLanguage(kPortugueseBrazilKey);
    CHECK(PortugueseBrazilSelected());
    CHECK(std::string(Tr("Play")) == "Jogar");
    CHECK(std::string(Tr("Save")) == "Salvar");
    CHECK(std::string(Tr("Quit")) == "Sair");
    CHECK(std::string(Tr("Display")) == "Tela");
    CHECK(std::string(Tr("Graphics")) == "Gráficos");
    CHECK(std::string(Tr("Gameplay")) == "Jogabilidade");
    CHECK(std::string(Tr("Controls")) == "Controles");
    CHECK(std::string(Tr("About")) == "Sobre");
    CHECK(std::string(Tr("Sponsor on GitHub")) == "Patrocinar no GitHub");
    CHECK(std::string(Tr("UPDATE NOW")) == "ATUALIZAR AGORA");
    CHECK(std::string(Tr("Connection test")) == "Teste de Conexão");
    CHECK(std::string(Tr("Copy")) == "Copiar");
    CHECK(std::string(Tr("Decline")) == "Recusar");
    for (const UiTextRow& row : table.rows) {
        CHECK(std::string(Tr(row.english.c_str())) == row.text);
    }

    SetActiveLanguage("english");
    CHECK(!PortugueseBrazilSelected());
    CHECK(std::string(Tr("Play")) == "Play");
    CHECK(std::string(Tr("Menu & Subtitles")) == "Menu & Subtitles");
    for (const char* key : {"spanish", "french", "german", "italian"}) {
        SetActiveLanguage(key);
        CHECK(std::string(Tr("Play")) == "Play");
        CHECK(std::string(Tr("Save")) == "Save");
    }

    const std::string unknown = "Not a launcher string 12345";
    SetActiveLanguage(kPortugueseBrazilKey);
    CHECK(std::string(Tr(unknown)) == unknown);
    CHECK(std::string(Tr(unknown.c_str())) == unknown);

    SetActiveLanguage("english");
    CHECK(std::string(Tr("Play")) == "Play");
    SetActiveLanguage(kPortugueseBrazilKey);
    CHECK(std::string(Tr("Play")) == "Jogar");
    SetActiveLanguage("english");
}

void TestFileDialogLabel() {
    using namespace localization;
    trg::SetTranslator(+[](const char* text) { return Tr(text); });
    CHECK(std::string(trg::Translate("All files")) == "All files");
    SetActiveLanguage(kPortugueseBrazilKey);
    CHECK(std::string(trg::Translate("All files")) == "Todos os arquivos");
    SetActiveLanguage("english");
    trg::SetTranslator(nullptr);
    CHECK(std::string(trg::Translate("All files")) == "All files");
    const std::string patch = ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "patches" / "trg-launcher" / "0005-ui-translator.patch");
    CHECK(patch.find("Widen(Translate(\"All files\"))") != std::string::npos);
    CHECK(patch.find("ShellQuote(std::string(Translate(\"All files\")) + \" | *\")") != std::string::npos);
}

void TestTextWithValues() {
    using namespace localization;
    CHECK(TrValue("Request sent to Alice.") == "Request sent to Alice.");
    SetActiveLanguage(kPortugueseBrazilKey);
    CHECK(TrValue("Request sent to Alice.") == "Solicitação enviada para Alice.");
    CHECK(TrValue("Alice is now your friend.") == "Alice agora é seu amigo.");
    CHECK(TrValue("Request sent to %n%s.") == "Solicitação enviada para %n%s.");
    CHECK(TrValue("The online service refused the request (busy).") == "O serviço online recusou a solicitação (busy).");
    CHECK(TrValue("No player has that friend code.") == "Nenhum jogador tem esse código de amigo.");
    CHECK(TrValue("Windows could not start it (error 5).") == "O Windows não conseguiu iniciá-lo (erro 5).");
    CHECK(TrValue("Connected \xC2\xB7 32 ms") == "Conectado \xC2\xB7 32 ms");
    CHECK(TrValue("Cannot write the update file a.bin.") == "Não foi possível gravar o arquivo de atualização a.bin.");
    CHECK(TrValue("Cannot write a.bin.") == "Não foi possível gravar a.bin.");
    CHECK(TrValue("Not a launcher string 12345") == "Not a launcher string 12345");

    using Router = net::ConnectionFacts::Router;
    const auto translated = [](net::ConnectionFacts facts) {
        for (const uint16_t port : {uint16_t(0), uint16_t(3658)}) {
            facts.udp_port = port;
            for (const net::ConnectionReport::Line& line : net::summarize_connection(facts).lines) {
                CHECK(std::string(Tr(line.label)) != line.label || line.label == "Relay");
                CHECK(TrValue(line.text) != line.text);
            }
        }
    };
    for (const int64_t latency : {int64_t(-1), int64_t(32)}) {
        net::ConnectionFacts facts;
        facts.lobby_ok = true;
        facts.lobby_latency_ms = latency;
        translated(facts);
        facts.udp_ok = facts.second_answer = facts.same_mapping = true;
        translated(facts);
        facts.second_answer = false;
        translated(facts);
        facts.second_answer = true;
        facts.same_mapping = false;
        translated(facts);
        facts.router = Router::kMapped;
        translated(facts);
        facts.router = Router::kMappedBehindNat;
        translated(facts);
        facts.relay_ok = true;
        translated(facts);
        facts.stun_off = true;
        translated(facts);
        facts.port_open = false;
        translated(facts);
    }
    SetActiveLanguage("english");
}

int main(int argc, char** argv) {
    // dcp-launcher-tests --check-game <app0> <image dir>: the readiness check on real folders, read-only.
    if (argc == 4 && std::string(argv[1]) == "--check-game") {
        const GameCheck check = CheckGameFiles({PathFromUtf8(argv[2]), PathFromUtf8(argv[3])});
        std::printf("%s%s%s\neboot.bin SHA-256 %s\n", StateName(check.state), check.detail.empty() ? "" : ": ",
                    check.detail.c_str(), check.eboot_sha256.c_str());
        return check.state == GameFileState::kValid ? 0 : 1;
    }
    // dcp-launcher-tests --log-settings <dcp.ini> <image dir> [<app0>]: the startup Settings summary, read-only.
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--log-settings") {
        dcp_log_settings_summary(argv[2], argv[3], argc == 5 ? argv[4] : nullptr, nullptr);
        return 0;
    }
    // dcp-launcher-tests --install <.pkg or game folder> <prepared root> <image root>: the launcher's installer without its
    // window.
    if (argc == 5 && std::string(argv[1]) == "--install") {
        InstallHooks hooks;
        hooks.phase = [](const char* phase) { std::printf("%s\n", phase); };
        hooks.progress = [last = -1](uint64_t done, uint64_t total) mutable {
            const int percent = total ? int(done * 100 / total) : 0;
            if (percent / 10 != last / 10) {
                std::printf("  %d%%\n", percent);
            }
            last = percent;
            return true;
        };
        const InstallResult result =
            InstallGame(PathFromUtf8(argv[2]), {PathFromUtf8(argv[3]), PathFromUtf8(argv[4])}, hooks);
        const InstallMessage message = DescribeInstall(result.status);
        std::printf("%s: %s%s%s (%zu files, %zu linked, %llu bytes)\n", message.title.c_str(), message.text.c_str(),
                    result.detail.empty() ? "" : " - ", result.detail.c_str(), result.files, result.linked,
                    static_cast<unsigned long long>(result.bytes));
        return result.ok() ? 0 : 1;
    }

    const fs::path base = fs::temp_directory_path() / ("dcp_launcher_tests_" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    TestSha256AndJson(base);
    TestGameFiles(base);
    TestFingerprintMatchesProfile();
    TestSettings(base);
    TestPaths();
    TestLogNumbering(base);
    TestDefaultLogFolders(base);
    TestEnvironmentVariables();
    TestCorrectnessStaysFixed();
    TestPngDecoder();
    TestInstaller(base);
    TestVideoCoreSettings(base);
    TestDefaultsAgree();
    TestDetailedLogging(base);
    TestLogsDirectory(base);
    TestLogRetention(base);
    TestWindowTitle();
    TestRenderResolution(base);
    TestSettingsSummary(base);
    TestResolutionProbe();
    TestTextLanguage(base);
    TestCheatSettings(base);
    TestOnlineCheatLockout(base);
    TestJapaneseNpcVoiceSetting(base);
    TestOnlineSettings(base);
    TestOnlinePage();
    TestConnectionTestGate(base);
    TestConnectionTestPanel(base);
    TestOverlayPanel();
    TestNoSettingsMenu();
    TestPingDisplay(base);
    TestOnlineClient(base);
    TestSkipOfferSettings(base);
    TestSkipIntroSettings(base);
    TestControllerRumbleSettings(base);
    TestUnlockGallerySettings(base);
    TestEarlyStoryOnlineSetting(base);
    TestJapaneseMenuButtonsSettings(base);
    TestPinnedBuild(base);
    TestOverlayProgramsSetting(base);
    TestGameProcess();
    TestLauncherRenderer();
    TestAspectRatio(base);
    TestTrueUltrawide(base);
    TestFrameRate(base);
    TestPresentation(base);
    TestRenderer(base);
    TestGraphicsQuality(base);
    TestGraphicsCard(base);
    TestLauncherWanted();
    TestPlayGate();
    TestPlacement();
    TestOptions();
    InGroup("navigation", TestPageNavigation);
    InGroup("icons", TestPageIcons);
    InGroup("licenses", TestIconLicenses);
    InGroup("scrollbar", TestStablePageWidth);
    InGroup("scrollbar", TestScrollbarSettling);
    InGroup("separators", TestRowSeparators);
    InGroup("separators", TestSeparatorPages);
    InGroup("separators", TestRowRhythm);
    InGroup("separators", TestRowKinds);
    InGroup("sidebar", TestSidebarProportions);
    InGroup("sidebar", TestSidebarLabels);
    InGroup("layout", TestChromeLayout);
    InGroup("layout", TestLauncherDpiLayout);
    InGroup("layout", TestFooterAndCards);
    InGroup("header-art", TestHeaderArt);
    InGroup("parity", TestSharedLauncherSource);
    TestKeyBindings(base);
    TestFileBrowser(base);
    TestControllerPrompts(base);
    TestInstalledTextLanguages(base);
    TestTextLanguageSwitching(base);
    TestBuiltInEnglish(base);
    TestBuiltInRussian(base);
    TestKeepBaseLanguages(base);
    TestNpcVoiceResources(base);
    TestGameTranslation(base);
    TestLatinGlyphs();
    TestComposedGlyphGeometry();
    TestTranslationAcrossSlots(base);
    TestInGameUiText();
    TestVisualPatchFallback(base);
    TestSafeMode(base);
    TestBuilds(base);
    TestCusa10209Graphics(base);
    TestUs101Graphics(base);
    TestBuildMetadata();
    TestLayers(base);
    TestUsBuild();
    TestEuropeBuild(base);
    TestLegacyMarker(base);
    TestInstalledGameName(base);
    TestLauncherLocalization();
    TestFileDialogLabel();
    TestTextWithValues();
    TestActivation(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path redirected = fs::temp_directory_path() / "dcp_launcher_tests_log"; // kept: the log stays open
    fs::remove_all(redirected, ec);
    TestLogRedirect(redirected);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    for (const auto& [name, failures] : g_groups) {
        std::printf("GROUP|launcher-%s|%s\n", name.c_str(), failures == 0 ? "PASS" : "FAIL");
    }
    runtime_log_flush();
    return g_failures ? 1 : 0;
}
