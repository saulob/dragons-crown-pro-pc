# The launcher: TRG Launcher (third_party/trg-launcher, fetched by tools\build.ps1) in a window of its own that is
# closed before the loader creates the game window.
set(TRG_DIR ${PROJECT_SOURCE_DIR}/third_party/trg-launcher)
if (NOT EXISTS ${TRG_DIR}/include/trg/launcher.h)
    message(FATAL_ERROR "TRG Launcher is missing: run tools\\build.ps1, which fetches it (gpu\\deps.txt).")
endif()
set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/gpu/third_party/imgui)

# TRG's settings file and platform helpers do not need Dear ImGui.
add_library(trg_core STATIC ${TRG_DIR}/src/settings.cpp ${TRG_DIR}/src/platform.cpp)
target_include_directories(trg_core PUBLIC ${TRG_DIR}/include)
target_compile_definitions(trg_core PRIVATE _CRT_SECURE_NO_WARNINGS)
if (WIN32)
    target_link_libraries(trg_core PRIVATE shell32 ole32 comdlg32)
else()
    target_link_libraries(trg_core PRIVATE Threads::Threads)
endif()

# The launcher draws with the Dear ImGui compiled into the video core library.
add_library(trg_launcher STATIC ${TRG_DIR}/src/launcher.cpp ${TRG_DIR}/src/headers.cpp)
target_include_directories(trg_launcher PUBLIC ${IMGUI_DIR})
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(trg_launcher PUBLIC trg_core dcpgpu)

# Written for C++17: in C++20 and later u8string() is a different type.
set_target_properties(trg_core trg_launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(dcp_imgui_sdl STATIC ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(dcp_imgui_sdl PUBLIC ${IMGUI_DIR})
target_link_libraries(dcp_imgui_sdl PUBLIC SDL3::SDL3)

# Game files (check, PKG unpacking, install, boot image), the trophies (src/trophies), the settings file, the Display options and the Controller prompts patch:
# no window needed (tests link only this).
find_package(ZLIB REQUIRED)
add_library(dcp_launcher_core STATIC boot_image.cpp controller_prompts.cpp cpk_patch.cpp file_browser.cpp game_files.cpp
    game_install.cpp game_patches.cpp game_profile.cpp options.cpp paths.cpp pkg_extract.cpp resolution.c settings.cpp trophy_content.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_data.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_key.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_pack.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_runtime.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_service.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_store.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_view.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trp_archive.cpp)
target_include_directories(dcp_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(dcp_launcher_core PUBLIC trg_core SDL3::SDL3 dcp_keys PRIVATE ZLIB::ZLIB)
# Hashes and ciphers: CNG on Windows, OpenSSL's libcrypto elsewhere.
if (WIN32)
    target_sources(dcp_launcher_core PRIVATE crypto_win.cpp)
    target_link_libraries(dcp_launcher_core PRIVATE bcrypt)
    set(DCP_LOG_SOURCE ${PROJECT_SOURCE_DIR}/src/runtime_log.c)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_sources(dcp_launcher_core PRIVATE crypto_openssl.cpp)
    target_link_libraries(dcp_launcher_core PRIVATE OpenSSL::Crypto)
    set(DCP_LOG_SOURCE ${PROJECT_SOURCE_DIR}/src/runtime_log_posix.c)
endif()

# The Controller prompts patch (prompts/xbox-prompts.dcpx) as a byte array, prompts_patch.h.
set(DCP_PROMPTS_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/prompts/xbox-prompts.dcpx)
set(DCP_PROMPTS_PATCH_SOURCE ${CMAKE_BINARY_DIR}/generated/prompts_patch_data.cpp)
add_custom_command(OUTPUT ${DCP_PROMPTS_PATCH_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${DCP_PROMPTS_PATCH} -DOUTPUT=${DCP_PROMPTS_PATCH_SOURCE} -DSYMBOL=kXboxPromptsPatch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${DCP_PROMPTS_PATCH} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    COMMENT "Embedding prompts/xbox-prompts.dcpx")

set(DCP_SPIDER_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/prompts/arachnophobia-spider.dcpx)
set(DCP_SPIDER_PATCH_SOURCE ${CMAKE_BINARY_DIR}/generated/spider_patch_data.cpp)
add_custom_command(OUTPUT ${DCP_SPIDER_PATCH_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${DCP_SPIDER_PATCH} -DOUTPUT=${DCP_SPIDER_PATCH_SOURCE} -DSYMBOL=kSpiderSafeModePatch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${DCP_SPIDER_PATCH} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    COMMENT "Embedding prompts/arachnophobia-spider.dcpx")

add_library(dcp_launcher STATIC host.cpp launcher.cpp package_browser.cpp problem_dialog.cpp shift_key.cpp trophy_page.cpp
    ${DCP_PROMPTS_PATCH_SOURCE} ${DCP_SPIDER_PATCH_SOURCE})
target_compile_definitions(dcp_launcher PRIVATE DCP_PORT_VERSION="${DCP_PORT_VERSION}")
target_include_directories(dcp_launcher PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_link_libraries(dcp_launcher PUBLIC dcp_launcher_core trg_launcher dcp_imgui_sdl ${CMAKE_DL_LIBS})

set_target_properties(dcp_launcher_core dcp_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(dcp_launcher_core PRIVATE -Wall -Wextra)
target_compile_options(dcp_launcher PRIVATE -Wall -Wextra)

# Page screenshots from a hidden launcher window (tests/launcher_shots.cpp); not part of the default build.
add_executable(dcp-launcher-shots EXCLUDE_FROM_ALL ../tests/launcher_shots.cpp)
target_link_libraries(dcp-launcher-shots PRIVATE dcp_launcher)
set_target_properties(dcp-launcher-shots PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Focused tests with synthetic data and temporary folders (tools\test.ps1); not part of the default build.
find_package(PNG REQUIRED)
add_executable(dcp-launcher-tests EXCLUDE_FROM_ALL ../tests/launcher_tests.cpp ${DCP_LOG_SOURCE}
    ../gpu/shim/dcp_settings.cpp ../gpu/shim/dcp_png.cpp)
target_compile_definitions(dcp-launcher-tests PRIVATE DCP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(dcp-launcher-tests PRIVATE dcp_launcher_core PNG::PNG)
target_include_directories(dcp-launcher-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
set_target_properties(dcp-launcher-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(dcp-launcher-tests PRIVATE -Wall -Wextra)

# Focused tests of the local trophies (tools/test.ps1); not part of the default build.
add_executable(dcp-trophy-tests EXCLUDE_FROM_ALL ../tests/trophy_tests.cpp ${DCP_LOG_SOURCE} ../gpu/shim/dcp_png.cpp)
target_link_libraries(dcp-trophy-tests PRIVATE dcp_launcher_core PNG::PNG)
target_include_directories(dcp-trophy-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(dcp-trophy-tests PRIVATE DCP_TROPHY_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/trophies")
set_target_properties(dcp-trophy-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(dcp-trophy-tests PRIVATE -Wall -Wextra)
