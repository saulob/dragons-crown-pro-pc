// Focused tests of the Proton/Wine SDL HIDAPI default (issue #42) and the Steam-launched Wayland window backend.
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#define DCP_SDL_COMPAT_POLICY_ONLY
#define DCP_VIDEO_BACKEND_POLICY_ONLY
#include "dcp_sdl_compat.h"
#include "dcp_video_backend.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

std::string ReadSource(const char* path) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

void TestPolicy() {
    CHECK(dcp_hidapi_action(0, nullptr) == DCP_HIDAPI_LEAVE);
    CHECK(dcp_hidapi_action(0, "") == DCP_HIDAPI_LEAVE);
    CHECK(dcp_hidapi_action(1, nullptr) == DCP_HIDAPI_DISABLE);
    CHECK(dcp_hidapi_action(1, "") == DCP_HIDAPI_DISABLE);
    CHECK(dcp_hidapi_action(1, "0") == DCP_HIDAPI_PRESERVE);
    CHECK(dcp_hidapi_action(1, "1") == DCP_HIDAPI_PRESERVE);
    CHECK(dcp_hidapi_action(0, "0") == DCP_HIDAPI_PRESERVE);
    CHECK(dcp_hidapi_action(0, "1") == DCP_HIDAPI_PRESERVE);
}

void TestProbeIsNotWineOnNative() {
#ifdef _WIN32
    CHECK(dcp_running_under_wine() == 0 || GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version"));
#else
    CHECK(dcp_running_under_wine() == 0);
#endif
}

static_assert(std::is_same_v<decltype(&dcp_hidapi_action), DcpHidapiAction (*)(int, const char*)>);
static_assert(std::is_same_v<decltype(&dcp_running_under_wine), int (*)(void)>);

size_t Pos(const std::string& text, const char* needle) {
    const size_t at = text.find(needle);
    if (at == std::string::npos) {
        std::printf("FAIL missing: %s\n", needle);
        ++g_failures;
    }
    return at;
}

size_t Count(const std::string& text, const char* needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

std::string LineBefore(const std::string& text, size_t at) {
    const size_t end = text.rfind('\n', at);
    const size_t start = text.rfind('\n', end - 1);
    return text.substr(start + 1, end - start - 1);
}

void TestNativeAndWineMatrix() {
    CHECK(dcp_hidapi_action(0, nullptr) == DCP_HIDAPI_LEAVE);
    CHECK(dcp_hidapi_action(1, nullptr) == DCP_HIDAPI_DISABLE);
    CHECK(dcp_hidapi_action(1, "0") == DCP_HIDAPI_PRESERVE);
    CHECK(dcp_hidapi_action(1, "1") == DCP_HIDAPI_PRESERVE);
}

void TestPolicyIsHostOnly() {
    const std::string header = ReadSource("src/dcp_sdl_compat.h");
    CHECK(!header.empty());
    for (const char* forbidden : {"language", "LANGUAGE", "game_build", "GameBuild", "title_id", "CUSA", "region", "eboot",
                                  "getenv", "WINEPREFIX", "STEAM_COMPAT", "28de", "11ff", "Bazzite", "Deck"}) {
        if (header.find(forbidden) != std::string::npos) {
            std::printf("FAIL header mentions %s\n", forbidden);
            ++g_failures;
        }
    }
    CHECK(Pos(header, "SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)") != std::string::npos);
    CHECK(Pos(header, "wine_get_version") != std::string::npos);
    CHECK(Pos(header, "#ifdef _WIN32") < Pos(header, "wine_get_version"));
}

void TestGameProcessOrder() {
    const std::string loader = ReadSource("src/loader.c");
    const size_t prepare = Pos(loader, "dcp_prepare_sdl_gamepad();");
    CHECK(Count(loader, "dcp_prepare_sdl_gamepad();") == 1);
    CHECK(LineBefore(loader, prepare).find("if") == std::string::npos);
    CHECK(Pos(loader, "dcp_launcher_run(&launcher)") < prepare);
    for (const char* later : {"dcp_game_build(paths.image_dir", "runtime_online_configure(", "dcp_render_resolution(",
                              "runtime_set_language(", "runtime_pad_set_settings(", "runtime_cheats_start(", "dcpgpu_init("}) {
        CHECK(prepare < Pos(loader, later));
    }
    CHECK(Pos(loader, "if (game_child) {") < prepare);
    CHECK(Pos(loader, "if (!cpu_only && !check_imports && !game_child) {") < prepare);

    const std::string window = ReadSource("gpu/shim/window.cpp");
    const size_t window_prepare = Pos(window, "dcp_prepare_sdl_gamepad();");
    CHECK(Count(window, "dcp_prepare_sdl_gamepad();") == 1);
    CHECK(window_prepare < Pos(window, "SDL_INIT_GAMEPAD"));
    CHECK(window.find("dcp_game_build") == std::string::npos && window.find("text_language") == std::string::npos);

    const std::string pad = ReadSource("src/runtime_pad.c");
    const size_t pad_prepare = Pos(pad, "dcp_prepare_sdl_gamepad();");
    CHECK(Count(pad, "dcp_prepare_sdl_gamepad();") == 1);
    CHECK(pad_prepare < Pos(pad, "SDL_InitSubSystem(SDL_INIT_GAMEPAD)"));
    CHECK(Count(pad, "dcp_hidapi_action") == 0 && Count(pad, "dcp_running_under_wine") == 0);
}

void TestLauncherSeparate() {
    const std::string host = ReadSource("launcher/host.cpp");
    CHECK(host.find("dcp_sdl_compat") == std::string::npos);
    CHECK(host.find("dcp_prepare_sdl_gamepad") == std::string::npos);
    CHECK(host.find("SDL_Quit()") != std::string::npos);
}

void TestEveryBuildAndLanguageSharesThePath() {
    const std::string profile = ReadSource("launcher/game_profile.cpp");
    for (const char* build : {"CUSA10454 1.00 USA", "CUSA10454 1.01 USA", "CUSA07001 1.00 Japan", "CUSA07001 1.02 Japan",
                              "CUSA07001 1.03 Japan", "CUSA10209 1.00 Asia/Hong Kong", "CUSA10209 1.02 Asia/Hong Kong",
                              "CUSA10487 1.03 Europe"}) {
        CHECK(Pos(profile, build) != std::string::npos);
    }
    for (const char* language : {"\"english\"", "\"french\"", "\"german\"", "\"italian\"", "\"spanish\"", "\"japanese\"",
                                 "\"traditional_chinese\"", "\"korean\""}) {
        CHECK(Pos(profile, language) != std::string::npos);
    }
    for (const char* file : {"src/runtime_pad.c", "launcher/host.cpp"}) {
        const std::string text = ReadSource(file);
        for (const char* key : {"CUSA10454", "CUSA07001", "CUSA10209", "CUSA10487", "DCP_LANGUAGE", "text_language", "dcp_game_build"}) {
            CHECK(text.find(key) == std::string::npos);
        }
    }
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(pad.find("runtime_language") == std::string::npos && pad.find("language") == std::string::npos);
}

void TestGamepadMappings() {
    char path[64];
    CHECK(dcp_gamepad_db_path("user/dcp.ini", path, sizeof(path)) && std::string(path) == "user/gamecontrollerdb.txt");
    CHECK(dcp_gamepad_db_path("C:\\a\\user\\dcp.ini", path, sizeof(path)) &&
          std::string(path) == "C:\\a\\user\\gamecontrollerdb.txt");
    CHECK(dcp_gamepad_db_path("C:/a\\b/dcp.ini", path, sizeof(path)) && std::string(path) == "C:/a\\b/gamecontrollerdb.txt");
    CHECK(dcp_gamepad_db_path("dcp.ini", path, sizeof(path)) && std::string(path) == "gamecontrollerdb.txt");
    CHECK(!dcp_gamepad_db_path("", path, sizeof(path)));
    CHECK(!dcp_gamepad_db_path(nullptr, path, sizeof(path)));
    char small[8];
    CHECK(!dcp_gamepad_db_path("user/dcp.ini", small, sizeof(small)));

    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(Pos(pad, "SDL_AddGamepadMappingsFromFile") != std::string::npos);
    CHECK(Pos(pad, "SDL_InitSubSystem(SDL_INIT_GAMEPAD)") < Pos(pad, "if (sdl_ready>0) load_user_gamepad_mappings();"));
    CHECK(Pos(pad, "joystick without a gamepad mapping") != std::string::npos);
    CHECK(Pos(pad, "SDL_GUIDToString") != std::string::npos);
    CHECK(pad.find("SDL_SetGamepadMapping") == std::string::npos); // nothing is guessed for an unknown device
}

void TestEnumerationUnchanged() {
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(Pos(pad, "#define MAX_PADS 4") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepads") != std::string::npos);
    CHECK(Pos(pad, "SDL_OpenGamepad") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepadButton") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepadAxis") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetGamepadTouchpadFinger") != std::string::npos);
    CHECK(Pos(pad, "DCP_PAD_MAP") != std::string::npos);
    CHECK(Pos(pad, "DCP_KEY_MAP") != std::string::npos);
    CHECK(Pos(pad, "SDL_GetKeyboardState") != std::string::npos);
    CHECK(Pos(pad, "free_slot") != std::string::npos);
    CHECK(Pos(pad, "Runtime: gamepad %d disconnected") != std::string::npos);
    CHECK(Pos(pad, "Steam Input virtual gamepad") != std::string::npos);
}

DcpVideoEnvironment SteamWayland() {
    DcpVideoEnvironment e{};
    e.linux_host = 1;
    e.steam_game_id = "12345678901234567";
    e.steam_app_id = "4023456789";
    e.session_type = "wayland";
    e.wayland_display = "wayland-0";
    e.display = ":0";
    e.x11_driver_listed = 1;
    return e;
}

std::vector<std::string> Telemetry(const DcpVideoEnvironment& e) {
    char lines[DCP_VIDEO_MAX_LINES][DCP_VIDEO_LINE_SIZE];
    const size_t n = dcp_video_telemetry(dcp_video_choose(&e), lines);
    return {lines, lines + n};
}

void TestVideoBackendChoice() {
    DcpVideoEnvironment e = SteamWayland();
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_USE_X11);

    e = SteamWayland();
    e.gamescope_display = "gamescope-0";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_KEEP_GAMESCOPE);
    e.display = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_KEEP_GAMESCOPE);

    e = SteamWayland();
    e.steam_game_id = e.steam_app_id = "1";
    e.gamescope_display = "gamescope-0";
    e.session_type = "x11";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_KEEP_GAMESCOPE);

    e = SteamWayland();
    e.session_type = "x11";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);
    e.session_type = "tty";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);

    e = SteamWayland();
    e.steam_game_id = e.steam_app_id = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);
    e.steam_game_id = e.steam_app_id = "";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);

    e = SteamWayland();
    e.steam_app_id = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_USE_X11);
    e = SteamWayland();
    e.steam_game_id = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_USE_X11);

    e = SteamWayland();
    e.linux_host = 0;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);

    e = SteamWayland();
    e.display = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_X11_UNAVAILABLE);
    e.display = "";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_X11_UNAVAILABLE);
    e = SteamWayland();
    e.x11_driver_listed = 0;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_X11_UNAVAILABLE);

    e = SteamWayland();
    e.session_type = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_USE_X11);
    e.wayland_display = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);
}

void TestVideoBackendUserPrecedence() {
    DcpVideoEnvironment e = SteamWayland();
    e.video_driver = "wayland";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_KEEP_USER_DRIVER);
    e.video_driver = "x11";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_KEEP_USER_DRIVER);
    e.video_driver = "";
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_USE_X11);
    e.video_driver = "wayland";
    e.steam_game_id = e.steam_app_id = nullptr;
    CHECK(dcp_video_choose(&e) == DCP_VIDEO_UNCHANGED);

    const std::string header = ReadSource("src/dcp_video_backend.h");
    CHECK(Count(header, "SDL_SetHint(") == 1);
    CHECK(Pos(header, "if (choice == DCP_VIDEO_USE_X11) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, DCP_VIDEO_X11_FIRST);") !=
          std::string::npos);
    CHECK(Pos(header, "SDL_GetHint(SDL_HINT_VIDEO_DRIVER)") != std::string::npos);
    CHECK(Count(header, "\"x11,wayland\"") == 1);
    CHECK(header.find("setenv") == std::string::npos && header.find("putenv") == std::string::npos);
}

void TestVideoBackendTelemetry() {
    DcpVideoEnvironment e = SteamWayland();
    const std::vector<std::string> selected = Telemetry(e);
    CHECK(selected.size() == 2);
    CHECK(selected.size() == 2 &&
          selected[0] == "Steam Input telemetry: event=steam_input_wayland_detected session=wayland backend=default "
                         "gamescope=no count=1");
    CHECK(selected.size() == 2 &&
          selected[1] == "Steam Input telemetry: event=steam_input_xwayland_selected session=wayland backend=x11 "
                         "gamescope=no count=1");

    e.display = nullptr;
    const std::vector<std::string> unavailable = Telemetry(e);
    CHECK(unavailable.size() == 2 && unavailable[1].find("event=steam_input_xwayland_unavailable ") != std::string::npos);
    CHECK(unavailable.size() == 2 && unavailable[1].find("backend=default") != std::string::npos);

    e = SteamWayland();
    e.gamescope_display = "gamescope-0";
    const std::vector<std::string> gamescope = Telemetry(e);
    CHECK(gamescope.size() == 1 &&
          gamescope[0] == "Steam Input telemetry: event=steam_input_native_wayland_retained session=wayland "
                          "backend=default gamescope=yes reason=gamescope count=1");

    e = SteamWayland();
    e.video_driver = "wayland";
    const std::vector<std::string> user = Telemetry(e);
    CHECK(user.size() == 2 && user[1].find("reason=user_driver") != std::string::npos);

    e = SteamWayland();
    e.session_type = "x11";
    CHECK(Telemetry(e).empty());
    e = SteamWayland();
    e.steam_game_id = e.steam_app_id = nullptr;
    CHECK(Telemetry(e).empty());
    e = SteamWayland();
    e.linux_host = 0;
    CHECK(Telemetry(e).empty());

    const std::regex safe("Steam Input telemetry: event=steam_input_(wayland_detected|xwayland_selected|"
                          "xwayland_unavailable|native_wayland_retained) session=wayland backend=(x11|default) "
                          "gamescope=(yes|no)( reason=(gamescope|user_driver))? count=1");
    for (DcpVideoChoice choice : {DCP_VIDEO_USE_X11, DCP_VIDEO_X11_UNAVAILABLE, DCP_VIDEO_KEEP_GAMESCOPE,
                                  DCP_VIDEO_KEEP_USER_DRIVER}) {
        char lines[DCP_VIDEO_MAX_LINES][DCP_VIDEO_LINE_SIZE];
        const size_t n = dcp_video_telemetry(choice, lines);
        CHECK(n >= 1 && n <= DCP_VIDEO_MAX_LINES);
        for (size_t i = 0; i < n; ++i) {
            const std::string line = lines[i];
            CHECK(std::regex_match(line, safe));
            for (const char* secret : {"12345678901234567", "4023456789", "wayland-0", "gamescope-0", ":0", "/", "\\"}) {
                CHECK(line.find(secret) == std::string::npos);
            }
        }
    }

    const std::string loader = ReadSource("src/loader.c");
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Count(loader, "dcp_video_backend_prepare(") == 1 && Count(loader, "dcp_video_backend_prepare(1)") == 1);
    CHECK(Count(window, "dcp_video_backend_prepare(") == 1 && Count(window, "dcp_video_backend_prepare(0)") == 1);
}

void TestVideoBackendOrderAndScope() {
    const std::string loader = ReadSource("src/loader.c");
    const size_t prepare = Pos(loader, "dcp_video_backend_prepare(1)");
    CHECK(Pos(loader, "#ifndef _WIN32\n    if (!cpu_only && !check_imports) dcp_video_backend_prepare(1);") != std::string::npos);
    CHECK(prepare < Pos(loader, "dcp_launcher_run(&launcher)"));
    CHECK(prepare < Pos(loader, "dcpgpu_init("));

    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Pos(window, "#ifndef _WIN32\n    dcp_video_backend_prepare(0);\n#endif") != std::string::npos);
    CHECK(Pos(window, "dcp_video_backend_prepare(0)") < Pos(window, "SDL_InitSubSystem(SDL_INIT_VIDEO"));
    CHECK(window.find("DEBUG_SAULO") == std::string::npos);

    const std::string header = ReadSource("src/dcp_video_backend.h");
    for (const char* forbidden : {"Vulkan", "VULKAN", "OPENGL", "SDL_GL", "touch", "Touch", "pointer", "Pointer", "GAMEPAD",
                                  "Gamepad", "mapping", "forceinputappid", "steam://", "VID:", "controller", "path"}) {
        if (header.find(forbidden) != std::string::npos) {
            std::printf("FAIL video backend header mentions %s\n", forbidden);
            ++g_failures;
        }
    }
    CHECK(header.find("DCP_VIDEO_BACKEND_POLICY_ONLY") != std::string::npos);

    for (const char* untouched : {"src/runtime_pad.c", "src/dcp_touchscreen.c", "src/dcp_mouse.c", "src/dcp_pad_owner.c",
                                  "src/dcp_pointer_motion.c", "gpu/shadps4/video_core/renderer_vulkan/vk_platform.cpp",
                                  "gpu/shim/dcp_touch_route.h", "launcher/host.cpp"}) {
        const std::string text = ReadSource(untouched);
        CHECK(!text.empty());
        CHECK(text.find("dcp_video_") == std::string::npos && text.find("steam_input_") == std::string::npos);
    }
    CHECK(window.find("SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);") != std::string::npos);
    CHECK(window.find("api == GraphicsApi::Vulkan ? SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN") != std::string::npos);
}

} // namespace

int main() {
    TestVideoBackendChoice();
    TestVideoBackendUserPrecedence();
    TestVideoBackendTelemetry();
    TestVideoBackendOrderAndScope();
    TestGamepadMappings();
    TestPolicy();
    TestProbeIsNotWineOnNative();
    TestNativeAndWineMatrix();
    TestPolicyIsHostOnly();
    TestGameProcessOrder();
    TestLauncherSeparate();
    TestEveryBuildAndLanguageSharesThePath();
    TestEnumerationUnchanged();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sdl compat tests passed (%d checks)\n", g_checks);
    return 0;
}
