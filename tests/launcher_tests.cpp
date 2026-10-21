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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "controller_prompts.h"
#include "game_patches.h"
#include "cpk_patch.h"
#include "dcp_cheats.h"
#include "dcp_launcher.h"
#include "dcp_graphics.h"
#include "dcp_keys.h"
#include "dcp_png.h"
#include "dcp_settings.h"
#include "dcp_title.h"
#include "file_browser.h"
#include "game_files.h"
#include "game_install.h"
#include "options.h"
#include "paths.h"
#include "placement.h"
#include "play_gate.h"
#include "runtime_log.h"
#include "settings.h"
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
          "CUSA10454 1.00 USA, CUSA10454 1.01 USA, CUSA07001 1.00 Japan, CUSA07001 1.02 Japan, CUSA07001 1.03 Japan, CUSA10209 1.00 Asia/Hong Kong and CUSA10209 1.02 Asia/Hong Kong");
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

    // A file the in-game menu wrote: "key=value" lines, keys the launcher does not manage, a comment.
    WriteAll(file,
             "# video core settings (in-game menu: Insert / R3+L2)\n"
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
    // Unix/macOS branches of vendored shadPS4 code, and the Linux AppImage's own path and desktop session.
    const std::vector<std::string> platform = {"HOME", "XDG_DATA_HOME", "VK_DRIVER_FILES", "APPIMAGE", "USER", "DISPLAY"};
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
// writes only the keys the in-game menu changes.
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
             "menu_tab = 1\n"
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
        CHECK(v.overlay_scale == 150 && v.overlay_corner == 3 && v.menu_tab == 1);
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
             "menu_tab = 2\n"
             "smaa = 1\n"
             "smaa = 0\n"
             "not a setting line\n"
             "= 1\n");
    {
        DcpSettings::Values v;
        CHECK(DcpSettings::Read(file.string(), v));
        CHECK(v.vsync && v.rcas && v.rcas_strength == 75 && v.anisotropy == 0);
        CHECK(v.display_mode == DcpGraphics::Windowed && v.graphics_card.empty() && v.monitor == 1);
        CHECK(v.requested_width == 0 && v.overlay_scale == 300 && v.overlay_corner == 1 && v.menu_tab == 0);
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

    // The menu's save and the F11 save rewrite their own keys in place and keep every other line.
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
    }
    live.vsync = false;
    live.rcas_strength = 30;
    live.show_fps = true;
    CHECK(DcpSettings::Save());
    {
        const std::string text = ReadAll(file);
        CHECK(text.find("vsync = 0") != std::string::npos);
        CHECK(text.find("rcas_strength = 30") != std::string::npos);
        CHECK(text.find("show_fps = 1") != std::string::npos);
        CHECK(text.find("launcher_width = 1280") != std::string::npos && text.find("show_launcher = 0") != std::string::npos);
        for (const char* never : {"smaa", "anisotropy", "graphics_card", "monitor", "window_width", "upscaler",
                                  "async_shaders", "fps_limit", "frames_queued"}) {
            CHECK(text.find(never) == std::string::npos);
        }
        LauncherSettings reread(file);
        CHECK(reread.Get(kDisplayModeKey) == "borderless" && !reread.GetBool(kVSyncKey, true));
        CHECK(reread.GetInt(kSharpenStrengthKey, 0) == 30 && reread.GetBool(kShowFrameCounterKey, false));
        CHECK(reread.GetBool(kShowLauncherKey, true) == false);
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
    CHECK(launcher_default(kGraphicsCardKey) == "automatic" && game.graphics_card.empty());
    CHECK(launcher_default(kMonitorKey) == std::to_string(game.monitor.load()));
    CHECK(launcher_default(kShowFpsTitleKey) == flag(game.show_fps_title));
    CHECK(launcher_default(kShowResolutionTitleKey) == flag(game.show_resolution_title));
    CHECK(launcher_default(kShowFrameCounterKey) == flag(game.show_fps));
    // 0 is "the loader's size", 1920 x 1080 (src/loader.c), the launcher's default.
    CHECK(game.requested_width == 0 && launcher_default(kWindowWidthKey) == "1920");
    CHECK(game.requested_height == 0 && launcher_default(kWindowHeightKey) == "1080");
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c").find(",1920,1080};") != std::string::npos);

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

    // A saved On is inert on every build without the verified hook.
    CHECK(!SupportsJapaneseNpcVoices(&kUs101));
    CHECK(!SupportsJapaneseNpcVoices(&kJp100));
    CHECK(!SupportsJapaneseNpcVoices(&kJp102));
    CHECK(!SupportsJapaneseNpcVoices(&kJp103));
    CHECK(!SupportsJapaneseNpcVoices(&kCk100));
    CHECK(!SupportsJapaneseNpcVoices(&kCk));
    CHECK(!SupportsJapaneseNpcVoices(nullptr));
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
        CHECK(dcp_saved_cheats(file.string().c_str()) != 0);
        CHECK(settings.Save());
    }
    CHECK(dcp_saved_cheats(file.string().c_str()) == 0);
    CHECK(dcp_saved_movement_speed(file.string().c_str()) == 1);

    // Experience Multiplier: the saved value is the multiplier; a value that is not listed is Off.
    CHECK(launcher_default(cheats::kExperienceMultiplierKey) == "1");
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

    // The in-game menu never writes them: the video core's settings file code has no cheat key.
    CHECK(ReadAll(fs::path(DCP_SOURCE_DIR) / "gpu" / "shim" / "dcp_settings.cpp").find("cheat") == std::string::npos);
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

    CHECK(dcp_text_language(ini.c_str(), us.c_str()) == 1); // no file: English
    CHECK(dcp_text_language(nullptr, nullptr) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str()) == 10); // no file: Traditional Chinese

    struct Expected { const char* key; const char* name; int id; };
    const Expected western[] = {{"english", "English", 1}, {"french", "French", 2}, {"german", "German", 4},
                                {"italian", "Italian", 5}, {"spanish", "Spanish", 3}};
    const Expected asian[] = {{"traditional_chinese", "Traditional Chinese", 10}, {"korean", "Korean", 9}};
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
        check_list(&build, std::string_view(build.title_id) == "CUSA10209" ? std::span<const Expected>(asian)
                                                                          : std::span<const Expected>(western));
    }

    for (const char* image : {us.c_str(), static_cast<const char*>(nullptr)}) {
        for (const Expected& language : western) {
            LauncherSettings settings(file);
            settings.Set(kTextLanguageKey, language.key);
            CHECK(settings.Save());
            CHECK(WrittenValue(ReadAll(file), "text_language", language.key));
            CHECK(dcp_text_language(ini.c_str(), image) == language.id);
        }
    }
    for (const Expected& language : asian) {
        LauncherSettings settings(file);
        settings.Set(kTextLanguageKey, language.key);
        CHECK(settings.Save());
        CHECK(dcp_text_language(ini.c_str(), asia.c_str()) == language.id);
    }

    // A saved language the active build does not have gives that build's default, and the file keeps it.
    WriteAll(file, "text_language=korean\n");
    CHECK(dcp_text_language(ini.c_str(), us.c_str()) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str()) == 9);
    for (const Expected& language : western) {
        WriteAll(file, std::string("text_language=") + language.key + "\n");
        CHECK(dcp_text_language(ini.c_str(), asia.c_str()) == 10);
        CHECK(dcp_text_language(ini.c_str(), us.c_str()) == language.id);
    }
    CHECK(ResolveTextLanguage(&kCk, "english").key == "traditional_chinese");
    CHECK(ResolveTextLanguage(&kCk, "korean").key == "korean");
    CHECK(ResolveTextLanguage(&kUs, "korean").key == "english");
    CHECK(ResolveTextLanguage(&kUs, "traditional_chinese").key == "english");
    CHECK(ResolveTextLanguage(&kJp100, "korean").key == "english");
    for (const char* absent : {"chinese", "simplified_chinese", "japanese"}) {
        CHECK(!IsValidSetting(kTextLanguageKey, absent));
        CHECK(ResolveTextLanguage(&kCk, absent).ps4_id == 10);
    }
    // Both CUSA10209 builds share one list: Traditional Chinese and Korean, no Western or Simplified Chinese language.
    for (const GameBuild* asia : {&kCk100, &kCk}) {
        const std::span<const TextLanguage> languages = TextLanguagesOf(asia);
        CHECK(languages.size() == 2 && languages[0].key == "traditional_chinese" && languages[1].key == "korean");
        CHECK(languages[0].ps4_id == 10 && languages[1].ps4_id == 9);
        for (const char* absent : {"english", "french", "german", "italian", "spanish", "simplified_chinese"}) {
            CHECK(std::none_of(languages.begin(), languages.end(),
                               [&](const TextLanguage& language) { return language.key == absent; }));
        }
    }

    for (const char* unlisted : {"English", "japanese", "fr", "2", ""}) {
        WriteAll(file, std::string("text_language=") + unlisted + "\n");
        CHECK(dcp_text_language(ini.c_str(), us.c_str()) == 1);
        CHECK(dcp_text_language(ini.c_str(), asia.c_str()) == 10);
    }
    WriteAll(file, "show_fps=1\n"); // an existing file without the key
    CHECK(dcp_text_language(ini.c_str(), us.c_str()) == 1);
    CHECK(dcp_text_language(ini.c_str(), asia.c_str()) == 10);
    CHECK(IsValidSetting(kTextLanguageKey, "german") && IsValidSetting(kTextLanguageKey, "korean") &&
          IsValidSetting(kTextLanguageKey, "traditional_chinese") && !IsValidSetting(kTextLanguageKey, "japanese"));

    LauncherSettings settings(file);
    settings.ResetToDefaults();
    CHECK(settings.Save());
    CHECK(WrittenValue(ReadAll(file), "text_language", "english"));
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

// Presentation only fits the finished frame into the window: its own key and default, Letterbox unless Stretch is chosen,
// and no change to the render target or the aspect ratio.
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
    const std::vector<std::string> names = {"NVIDIA GeForce RTX 4070", "", "Intel(R) UHD Graphics 730"};
    CHECK(DcpGraphics::FindGpuByName(names, "") == -1);
    CHECK(DcpGraphics::FindGpuByName(names, "automatic") == -1);
    CHECK(DcpGraphics::FindGpuByName(names, "NVIDIA GeForce RTX 4070") == 0);
    CHECK(DcpGraphics::FindGpuByName(names, "Intel(R) UHD Graphics 730") == 2);
    CHECK(DcpGraphics::FindGpuByName(names, "Intel(R) UHD Graphics 7") == -1); // exact names only
    CHECK(DcpGraphics::FindGpuByName(names, "AMD Radeon RX 7800 XT") == -1);   // removed card: Automatic
    CHECK(DcpGraphics::FindGpuByName({"", "Intel(R) UHD Graphics 730"}, "") == -1);

    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed |= setting.key == "graphics_card" && setting.default_value == "automatic";
    }
    CHECK(managed);
    {
        LauncherSettings settings(file);
        CHECK(settings.Get(kGraphicsCardKey, kAutomaticGpu) == "automatic");
        settings.Set(kGraphicsCardKey, "Intel(R) UHD Graphics 730");
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), "graphics_card", "Intel(R) UHD Graphics 730"));
    CHECK(LauncherSettings(file).Get(kGraphicsCardKey, kAutomaticGpu) == "Intel(R) UHD Graphics 730");
    {
        LauncherSettings settings(file);
        settings.ResetToDefaults();
        CHECK(settings.Save());
    }
    CHECK(WrittenValue(ReadAll(file), "graphics_card", "automatic"));

    const std::string instance = ReadAll(source / "gpu" / "shadps4" / "video_core" / "renderer_vulkan" / "vk_instance.cpp");
    CHECK(instance.find("FindGpuByName(names, requested)") != std::string::npos);
    CHECK(instance.find("IsDeviceUsable(physical) ? ") != std::string::npos);
    CHECK(instance.find("const bool left_is_discrete") != std::string::npos); // the Automatic ranking is intact
    CHECK(instance.find("IsDeviceUsable(physical));") != std::string::npos); // usable devices rank first
    const std::string launcher = ReadAll(source / "launcher" / "launcher.cpp");
    const size_t monitor = launcher.find("ui.Row(\"Monitor\"");
    const size_t card = launcher.find("ui.Row(\"Graphics card\"");
    const size_t vsync = launcher.find("ui.Toggle(\"VSync\"");
    CHECK(monitor != std::string::npos && monitor < card && card < vsync);
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
    Display display;
    display.usable = {0, 0, 1920, 1040};
    display.bounds = {0, 0, 1920, 1080};
    display.scale = 1.0f;
    CHECK(WindowSizeFits({1280, 720}, display));
    CHECK(WindowSizeFits({1600, 900}, display));
    CHECK(!WindowSizeFits({1920, 1080}, display)); // the title bar leaves no room on a 1080p desktop
    CHECK(!WindowSizeFits({2560, 1440}, display));
    const WindowSize fitted = FitGameWindow({1920, 1080}, display);
    CHECK(fitted.width <= 1920 && fitted.height <= 1040 - 40);
    CHECK(fitted.width * 9 / 16 >= fitted.height - 1 && fitted.width * 9 / 16 <= fitted.height + 1); // shape kept

    Display large;
    large.usable = {0, 0, 3840, 2100};
    large.bounds = {0, 0, 3840, 2160};
    large.scale = 2.25f;
    CHECK(WindowSizeFits({3840, 2160}, large) == false);
    CHECK(WindowSizeFits({2560, 1440}, large));
    CHECK(FitGameWindow({1920, 1080}, large) == WindowSize({1920, 1080}));

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
    CHECK(count == 8);
    CHECK(std::string(PageName(kPageOrder[0])) == "Play");
    CHECK(std::string(PageName(kPageOrder[2])) == "Graphics");
    CHECK(std::string(PageName(kPageOrder[3])) == "Gameplay");
    CHECK(std::string(PageName(kPageOrder[4])) == "Cheats"); // directly below Gameplay
    CHECK(std::string(PageName(kPageOrder[5])) == "Controls");
    CHECK(std::string(PageName(kPageOrder[6])) == "Trophies"); // after Controls, before About
    CHECK(std::string(PageName(kPageOrder[7])) == "About");
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
    CHECK(!kJp100.cheats && !kJp100.xbox_prompts && !kJp102.cheats && !kJp102.xbox_prompts && !kJp103.cheats &&
          !kJp103.xbox_prompts);
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
        const fs::path prompts = root / "jp-prompts";
        WriteAll(prompts / "DRACRO_PATCH_JP.CPK", "jp archive");
        WriteAll(prompts / "DRACRO.CPK", "shared archive");
        CHECK(PromptArchives(prompts, kJp103).empty() && InstalledPromptStyle(prompts, kJp103, {}) == PromptStyle::kPlayStation);
        CHECK(!SetPromptStyle(prompts, kJp103, PromptStyle::kXbox, {}).ok);
        CHECK(ReadAll(prompts / "DRACRO_PATCH_JP.CPK") == "jp archive" && ReadAll(prompts / "DRACRO.CPK") == "shared archive");
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
    CHECK(!kUs101.japanese_npc_voices && !SupportsJapaneseNpcVoices(&kUs101));
    CHECK(!SupportsJapaneseNpcVoices(nullptr));

    CHECK(std::string(kUs101.label) == "CUSA10454 1.01 USA" && std::string(kUs101.version) == "1.01");
    CHECK(std::string(kUs101.folder) == "CUSA10454-1.01" && std::string(kUs101.folder) != kUs.folder);
    CHECK(kUs101.layers.size() == 2 && !kUs101.layers[0].update && kUs101.layers[1].update);
    CHECK(std::string(FinishedLayer(kUs101).app_ver) == "01.01" &&
          std::string(FinishedLayer(kUs101).sfo_version) == "01.03");
    CHECK(std::string(kUs101.eboot_sha256) == "434b3bb43f407990f7b861ed57b8007ba459d63af25b1511ce7489be94c908c0");
    CHECK(std::string(kUs101.eboot_sha256) != kUs.eboot_sha256);
    CHECK(!kUs101.cheats && !kUs101.xbox_prompts && !kUs101.japanese_npc_voices && kUs101.resolution_sites.empty());
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(build.japanese_npc_voices == (&build == &kUs));
        CHECK(SupportsJapaneseNpcVoices(&build) == (&build == &kUs));
    }
    CHECK(BaseBuildOf(kUs101) == &kUs && BaseBuildOf(kUs) == nullptr);
    for (const GameBuild& build : SupportedBuilds()) {
        CHECK((build.cheats || build.xbox_prompts) == (&build == &kUs));
    }

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
    CHECK(launcher.find("The USA 1.01, Japanese 1.02 and 1.03 and Asia/Hong Kong 1.02 updates are installed over their respective 1.00 base packages.") != std::string::npos);
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

    // Makes builds[active] the newest prepared build; the others are older.
    const auto make_active = [&](size_t active) {
        const auto now = fs::file_time_type::clock::now();
        for (size_t i = 0; i < builds.size(); ++i) {
            fs::last_write_time(BuildPaths(roots, builds[i]).app0 / kPreparedMarker,
                                i == active ? now : now - std::chrono::hours(1));
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
    CHECK(!SetGamePatches(app0, kJp103, PromptStyle::kXbox, true, Span(xbox), Span(safe)).ok && InstalledSafeMode(app0));
    CHECK(SetGamePatches(app0, kJp103, PromptStyle::kPlayStation, false, {}, {}).ok && ReadAll(app0 / "DRACRO.CPK") == original);
}

void TestBuilds(const fs::path& base) {
    CHECK(SupportedBuilds().size() == 7);
    CHECK(std::string(kUs.title_id) == "CUSA10454" && std::string(kCk.title_id) == "CUSA10209");
    CHECK(std::string(kUs.folder) == "v1.03" && std::string(kCk.folder) == "CUSA10209-1.02");
    CHECK(std::string(kCk.content_id) == "HP0177-CUSA10209_00-DRAGONSCROWN0001");
    CHECK(std::string(FinishedLayer(kCk).app_ver) == "01.02" && std::string(FinishedLayer(kCk).sfo_version) == "01.00" && std::string(kCk.version) == "1.02");
    CHECK(std::string(kCk.eboot_sha256) == "927a3d0f73284b52e87e6f22dc4ecbea6350450182a94c1dc20d76907b086ac5");
    CHECK(std::string(kCk.regional_archive) == "DRACRO_CK.CPK" && std::string(kUs.regional_archive) == "DRACRO_US.CPK");
    CHECK(!kCk.cheats && !kCk.xbox_prompts && kUs.cheats && kUs.xbox_prompts);
    // 1.00 and 1.02 are different executables: only 1.00 has verified render resolution sites.
    CHECK(kUs.resolution_sites.size() == 23 && kCk.resolution_sites.empty());
    CHECK(kCk100.resolution_sites.size() == 23);
    CHECK(kCk100.resolution_sites.front().offset == 0x5978a8 && kCk100.resolution_sites.back().offset == 0x598128);
    CHECK(!RestrictedToNativeResolution(&kUs) && !RestrictedToNativeResolution(&kCk100));
    CHECK(RestrictedToNativeResolution(&kCk) && !RestrictedToNativeResolution(nullptr));

    CHECK(CheckSfoIdentity(SfoFor(kCk)).build == &kCk);
    CHECK(CheckSfoIdentity(SfoFor(kCk100)).build == &kCk100);
    CHECK(std::string(kCk100.label) == "CUSA10209 1.00 Asia/Hong Kong" && std::string(kCk100.folder) == "CUSA10209-1.00");
    CHECK(std::string(FinishedLayer(kCk100).app_ver) == "01.00" && std::string(FinishedLayer(kCk100).sfo_version) == "01.02");
    CHECK(std::string(kCk100.eboot_sha256) == "d37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b");
    CHECK(std::string(kCk100.eboot_sha256) != kCk.eboot_sha256 && std::string(kCk100.regional_archive) == "DRACRO_CK.CPK");
    CHECK(!kCk100.cheats && !kCk100.xbox_prompts);
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
    WriteAll(report_dir / "image.json", "{\n  \"title_id\": \"CUSA10209\",\n  \"eboot_sha256\": \"" + std::string(kCk.eboot_sha256) + "\",\n  \"app_ver\": \"01.02\"\n}");
    DcpGameBuild ck_game{};
    CHECK(dcp_game_build(report_dir.string().c_str(), &ck_game) == 1);
    CHECK(ck_game.cheats == 0 && ck_game.resolution_site_count == 0 && ck_game.resolution_sites == nullptr);

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
        const fs::path app0 = base / "ck-prompts";
        fs::remove_all(app0);
        WriteAll(app0 / "DRACRO_CK.CPK", "ck archive");
        WriteAll(app0 / "DRACRO.CPK", "shared archive");
        CHECK(PromptArchives(app0, kCk).empty() && InstalledPromptStyle(app0, kCk, {}) == PromptStyle::kPlayStation);
        CHECK(!SetPromptStyle(app0, kCk, PromptStyle::kXbox, {}).ok);
        CHECK(SetPromptStyle(app0, kCk, PromptStyle::kPlayStation, {}).ok);
        CHECK(ReadAll(app0 / "DRACRO_CK.CPK") == "ck archive" && ReadAll(app0 / "DRACRO.CPK") == "shared archive");
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

// CUSA10209 1.02 has no verified render resolution sites, so only the native render resolution and 16:9 are effective,
// whatever `dcp.ini` saved; CUSA10209 1.00 keeps the whole list.
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
    CHECK(dcp_render_resolution(file.string().c_str(), dir102.string().c_str()) == 0); // 1.02 falls back to native
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir102.string().c_str()) == 0);      // and to 16:9
    CHECK(dcp_render_resolution(file.string().c_str(), dir100.string().c_str()) != 0); // 1.00 keeps them
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir100.string().c_str()) != 0);
    CHECK(dcp_render_resolution(file.string().c_str(), nullptr) != 0);                 // the unknown build too
    CHECK(dcp_aspect_ratio(file.string().c_str(), nullptr) != 0);

    WriteAll(file, "render_resolution=native\naspect_ratio=16:9\n");
    CHECK(dcp_render_resolution(file.string().c_str(), dir102.string().c_str()) == 0);
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir102.string().c_str()) == 0);
}

// CUSA10454 1.01 has no verified render resolution sites, so only the native render resolution and 16:9 are effective;
// CUSA10454 1.00 keeps the whole list.
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
    CHECK(RestrictedToNativeResolution(&kUs101) && !RestrictedToNativeResolution(&kUs));
    CHECK(kUs101.resolution_sites.empty() && kUs.resolution_sites.size() == 23);
    CHECK(kUs101.resolution_sites.data() != kUs.resolution_sites.data());

    WriteAll(file, "render_resolution=4K\naspect_ratio=21:9\n");
    CHECK(dcp_render_resolution(file.string().c_str(), dir101.string().c_str()) == 0); // 1.01 falls back to native
    CHECK(dcp_aspect_ratio(file.string().c_str(), dir101.string().c_str()) == 0);      // and to 16:9
    CHECK(dcp_render_resolution(file.string().c_str(), dir100.string().c_str()) != 0); // 1.00 keeps them
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

int main(int argc, char** argv) {
    // dcp-launcher-tests --check-game <app0> <image dir>: the readiness check on real folders, read-only.
    if (argc == 4 && std::string(argv[1]) == "--check-game") {
        const GameCheck check = CheckGameFiles({PathFromUtf8(argv[2]), PathFromUtf8(argv[3])});
        std::printf("%s%s%s\neboot.bin SHA-256 %s\n", StateName(check.state), check.detail.empty() ? "" : ": ",
                    check.detail.c_str(), check.eboot_sha256.c_str());
        return check.state == GameFileState::kValid ? 0 : 1;
    }
    // dcp-launcher-tests --log-gameplay <dcp.ini> <image dir>: the startup Gameplay summary, read-only.
    if (argc == 4 && std::string(argv[1]) == "--log-gameplay") {
        dcp_log_gameplay_settings(argv[2], argv[3]);
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
    TestTextLanguage(base);
    TestCheatSettings(base);
    TestJapaneseNpcVoiceSetting(base);
    TestAspectRatio(base);
    TestPresentation(base);
    TestGraphicsQuality(base);
    TestGraphicsCard(base);
    TestLauncherWanted();
    TestPlayGate();
    TestPlacement();
    TestOptions();
    TestKeyBindings(base);
    TestFileBrowser(base);
    TestControllerPrompts(base);
    TestSafeMode(base);
    TestBuilds(base);
    TestCusa10209Graphics(base);
    TestUs101Graphics(base);
    TestBuildMetadata();
    TestLayers(base);
    TestUsBuild();
    TestLegacyMarker(base);
    TestInstalledGameName(base);
    TestActivation(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path redirected = fs::temp_directory_path() / "dcp_launcher_tests_log"; // kept: the log stays open
    fs::remove_all(redirected, ec);
    TestLogRedirect(redirected);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    runtime_log_flush();
    return g_failures ? 1 : 0;
}
