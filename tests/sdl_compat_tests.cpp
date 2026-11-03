// Focused tests of the Proton/Wine SDL HIDAPI default (issue #42).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

#define DCP_SDL_COMPAT_POLICY_ONLY
#include "dcp_sdl_compat.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
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
                              "CUSA07001 1.03 Japan", "CUSA10209 1.00 Asia/Hong Kong", "CUSA10209 1.02 Asia/Hong Kong"}) {
        CHECK(Pos(profile, build) != std::string::npos);
    }
    for (const char* language : {"\"english\"", "\"french\"", "\"german\"", "\"italian\"", "\"spanish\"", "\"japanese\"",
                                 "\"traditional_chinese\"", "\"korean\""}) {
        CHECK(Pos(profile, language) != std::string::npos);
    }
    for (const char* file : {"src/runtime_pad.c", "launcher/host.cpp"}) {
        const std::string text = ReadSource(file);
        for (const char* key : {"CUSA10454", "CUSA07001", "CUSA10209", "DCP_LANGUAGE", "text_language", "dcp_game_build"}) {
            CHECK(text.find(key) == std::string::npos);
        }
    }
    const std::string pad = ReadSource("src/runtime_pad.c");
    CHECK(pad.find("runtime_language") == std::string::npos && pad.find("language") == std::string::npos);
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

} // namespace

int main() {
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
    std::printf("sdl compat tests passed\n");
    return 0;
}
