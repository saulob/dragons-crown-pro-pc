cmake_minimum_required(VERSION 3.24)
project(dragons_crown_pro_pc VERSION 0.8.0 LANGUAGES C CXX)
set(DCP_PORT_VERSION "${PROJECT_VERSION}")

if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Build with Clang: MSYS2 CLANG64 (tools\\build.ps1) on Windows, tools/build.sh on Linux.")
endif()
if (NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"))
    message(FATAL_ERROR "Supported targets: Windows x64 and Linux x86_64.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if (WIN32)
    set(CMAKE_C_EXTENSIONS OFF)
else()
    set(CMAKE_C_EXTENSIONS ON) # POSIX interfaces (sigjmp_buf, memfd_create) are hidden in strict ISO mode
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Repository paths in __FILE__ and std::source_location stay out of the binary; debug info keeps them.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=>")
if (NOT WIN32)
    # Non-PIE: the heap handed to the guest (thread handles, TLS) must stay below 1 TiB (loader.c).
    set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-pie>")
    add_link_options(-no-pie)
endif()

if (NOT PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "project VERSION must start with MAJOR.MINOR.PATCH")
endif()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/dcp_version.h CONTENT "#define DCP_VERSION_NUMBERS \
${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0\n#define DCP_FILE_VERSION_STRING \
\"${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0\"\n#define DCP_VERSION_STRING \"${PROJECT_VERSION}\"\n")

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)

# Keyboard bindings shared by the launcher, the runtime pad and the video core.
add_library(dcp_keys STATIC src/dcp_keys.c)
target_include_directories(dcp_keys PUBLIC src)
target_link_libraries(dcp_keys PUBLIC SDL3::SDL3)
target_compile_options(dcp_keys PRIVATE -Wall -Wextra -Werror)

# Online play (docs/ONLINE.md): lobby link, identity, STUN, UPnP and the direct peer transport. No game code: the
# launcher, the runtime and tests/net_tests.cpp share it.
add_library(dcp_net STATIC src/net/addr.cpp src/net/connection_test.cpp src/net/crypto.cpp src/net/identity.cpp
    src/net/json.cpp src/net/lobby_link.cpp src/net/online_config.cpp src/net/peer_socket.cpp src/net/stun.cpp
    src/net/upnp.cpp src/net/websocket.cpp $<IF:$<BOOL:${WIN32}>,src/net/websocket_win.cpp,src/net/websocket_posix.cpp>)
target_include_directories(dcp_net PUBLIC src)
set_target_properties(dcp_net PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(dcp_net PRIVATE -Wall -Wextra)
if (WIN32)
    target_link_libraries(dcp_net PUBLIC winhttp ws2_32 bcrypt)
else()
    find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
    target_link_libraries(dcp_net PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()

add_subdirectory(gpu)
add_subdirectory(launcher)

set(DCP_RUNTIME_SOURCES
    src/runtime.c
    src/runtime_audio.c
    src/runtime_content.c
    src/runtime_dcp.c
    src/runtime_eventflag.c
    src/runtime_fiber.c
    src/runtime_file.c
    src/runtime_font.c
    src/runtime_kernel.c
    src/runtime_memory.c
    src/runtime_matching2.cpp
    src/runtime_mutex.c
    src/runtime_online.cpp
    src/runtime_pad.c
    src/runtime_rtc.c
    src/runtime_rudp.cpp
    src/runtime_rwlock.c
    src/runtime_savedata.c
    src/runtime_session_log.c
    src/runtime_sema.c
    src/runtime_services.c
    src/runtime_thread.c
    src/runtime_time.c)
# The built-in system fonts are assembled into runtime_font.c (.incbin): rebuild it when a font changes.
set(DCP_NOTO_SANS ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSans-Regular.ttf)
set(DCP_NOTO_SANS_TC ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansTC-Regular.otf)
set_source_files_properties(src/runtime_font.c PROPERTIES
    COMPILE_DEFINITIONS "DCP_NOTO_SANS_PATH=\"${DCP_NOTO_SANS}\";DCP_NOTO_SANS_TC_PATH=\"${DCP_NOTO_SANS_TC}\""
    OBJECT_DEPENDS "${DCP_NOTO_SANS};${DCP_NOTO_SANS_TC}")

if (WIN32)
    enable_language(RC)
    # Focused tests of the sparse guest memory backing (tools/test.ps1); not part of the default build.
    add_executable(dcp-memory-windows-tests EXCLUDE_FROM_ALL tests/memory_windows_tests.c)
    target_include_directories(dcp-memory-windows-tests PRIVATE src)
    target_compile_definitions(dcp-memory-windows-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_compile_options(dcp-memory-windows-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(dcp-memory-windows-tests PRIVATE onecore -pthread)
    target_link_options(dcp-memory-windows-tests PRIVATE -Wl,--disable-high-entropy-va)
    # Relink when the application icon, the manifest or the version changes.
    set_source_files_properties(src/dragons_crown_pro.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/packaging/icons/app-icon.ico;${CMAKE_SOURCE_DIR}/src/dragons_crown_pro.manifest;${CMAKE_BINARY_DIR}/generated/dcp_version.h")
    set(DCP_PLATFORM_SOURCES src/compat_win.c src/module_diagnostics.c src/runtime_log.c src/dragons_crown_pro.rc)
else()
    set(DCP_PLATFORM_SOURCES src/runtime_log_posix.c)
endif()
set(DCP_MAIN_SOURCES src/loader.c src/cheat_hooks.cpp src/guest_hook.c src/isa_emulate.c src/skip_offers.c src/startup_save_hook.c src/ultrawide_hooks.c src/voice_hooks.c src/runtime_cheats.cpp ${DCP_RUNTIME_SOURCES} ${DCP_PLATFORM_SOURCES})
if (WIN32)
    add_executable(dragons_crown_pro WIN32 ${DCP_MAIN_SOURCES})
else()
    add_executable(dragons_crown_pro ${DCP_MAIN_SOURCES})
endif()
set_target_properties(dragons_crown_pro PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dragons_crown_pro PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
target_compile_options(dragons_crown_pro PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
target_compile_options(dragons_crown_pro PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
if (WIN32)
    # Windows 10 1903+ APIs (VirtualAlloc2, MapViewOfFile3).
    target_compile_definitions(dragons_crown_pro PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    # The PS4 user range (64 GiB - 1 TiB) must stay free of host allocations: no high-entropy ASLR.
    # The guest main thread runs on the process stack: 16 MiB, committed (guest code has no stack probes).
    target_link_options(dragons_crown_pro PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    target_link_libraries(dragons_crown_pro PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    # -rdynamic: fault reports name host symbols (dladdr).
    target_link_options(dragons_crown_pro PRIVATE -rdynamic)
    target_link_libraries(dragons_crown_pro PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# The launcher reads its images next to the executable.
add_custom_command(TARGET dragons_crown_pro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dragons_crown_pro>/assets/launcher"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header-art.png"
        "$<TARGET_FILE_DIR:dragons_crown_pro>/assets/launcher/header-art.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/sponsor-heart.png"
        "$<TARGET_FILE_DIR:dragons_crown_pro>/assets/launcher/sponsor-heart.png"
    VERBATIM)

if (WIN32)
    # GUI-subsystem processes start with closed standard streams; the log must still work. Not part of the default build.
    add_executable(dcp-log-tests EXCLUDE_FROM_ALL tests/log_tests.c src/runtime_log.c src/runtime_session_log.c)
    target_include_directories(dcp-log-tests PRIVATE src)
    target_compile_options(dcp-log-tests PRIVATE -Wall -Wextra -Werror)
endif()

# Focused tests of the online layer: JSON, identity, STUN, the lobby link against a local lobby and the peer transport
# over loopback (tools/test.ps1); not part of the default build.
add_executable(dcp-net-tests EXCLUDE_FROM_ALL tests/net_tests.cpp)
target_link_libraries(dcp-net-tests PRIVATE dcp_net)
set_target_properties(dcp-net-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(dcp-net-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-net-tests PRIVATE -Wall -Wextra)
# The game's side of online play (Matching2, libSceNet, Rudp entry points) against a development lobby; skipped unless
# DCP_TEST_LOBBY names one. Not part of the default build.
add_executable(dcp-online-hle-tests EXCLUDE_FROM_ALL tests/online_hle_tests.cpp src/runtime_matching2.cpp src/runtime_rudp.cpp)
target_link_libraries(dcp-online-hle-tests PRIVATE dcp_net)
set_target_properties(dcp-online-hle-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(dcp-online-hle-tests PRIVATE -Wall -Wextra)

# Focused tests of the Story and Tutorial Skip Offers, on synthetic images and the local boot images (tools/test.ps1);
# not part of the default build.
add_executable(dcp-skip-offers-tests EXCLUDE_FROM_ALL tests/skip_offers_tests.c src/skip_offers.c)
target_include_directories(dcp-skip-offers-tests PRIVATE src)
target_compile_definitions(dcp-skip-offers-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-skip-offers-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the MOVBE and BMI1 emulation used when the host CPU lacks the instructions.
add_executable(dcp-isa-tests EXCLUDE_FROM_ALL tests/isa_tests.c src/isa_emulate.c)
target_include_directories(dcp-isa-tests PRIVATE src)
target_compile_options(dcp-isa-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the cheats: the list, the session layer, the menu model, the rules and the native hook layer
# (tools/test.ps1); not part of the default build.
set(DCP_IMGUI_TEST_SOURCES
    gpu/third_party/imgui/imgui.cpp gpu/third_party/imgui/imgui_draw.cpp
    gpu/third_party/imgui/imgui_tables.cpp gpu/third_party/imgui/imgui_widgets.cpp)
set_source_files_properties(${DCP_IMGUI_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
add_executable(dcp-cheats-tests EXCLUDE_FROM_ALL tests/cheats_tests.cpp src/cheat_hooks.cpp src/guest_hook.c src/runtime_cheats.cpp
    ${DCP_IMGUI_TEST_SOURCES})
set_target_properties(dcp-cheats-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-cheats-tests PRIVATE src gpu/third_party/imgui)
target_compile_definitions(dcp-cheats-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-cheats-tests PRIVATE -Wall -Wextra -fno-omit-frame-pointer)

# Focused tests of the text dialog and the system font set through the import resolver (tools/test.ps1,
# tools/build.sh --test); not part of the default build.
add_executable(dcp-text-tests EXCLUDE_FROM_ALL tests/text_tests.c src/runtime_cheats.cpp ${DCP_RUNTIME_SOURCES}
    $<$<BOOL:${WIN32}>:src/compat_win.c src/runtime_log.c> $<$<NOT:$<BOOL:${WIN32}>>:src/runtime_log_posix.c>)
set_target_properties(dcp-text-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-text-tests PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(dcp-text-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-text-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_compile_definitions(dcp-text-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(dcp-text-tests PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(dcp-text-tests PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Focused tests of libSceRtc through the import resolver (tools/test.ps1, tools/build.sh --test); not part of the default
# build.
add_executable(dcp-rtc-tests EXCLUDE_FROM_ALL tests/rtc_tests.c src/runtime_cheats.cpp ${DCP_RUNTIME_SOURCES}
    $<$<BOOL:${WIN32}>:src/compat_win.c src/runtime_log.c> $<$<NOT:$<BOOL:${WIN32}>>:src/runtime_log_posix.c>)
set_target_properties(dcp-rtc-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-rtc-tests PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
target_compile_options(dcp-rtc-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_compile_definitions(dcp-rtc-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(dcp-rtc-tests PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(dcp-rtc-tests PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Focused tests of the GPU tiling parameters and macro-tiled addresses against the game's own tiler (tools/test.ps1);
# not part of the default build.
add_executable(dcp-tiling-tests EXCLUDE_FROM_ALL tests/tiling_tests.cpp)
set_target_properties(dcp-tiling-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-tiling-tests PRIVATE gpu/shim gpu/shadps4 tests src ${CMAKE_BINARY_DIR}/gpu/include
    gpu/third_party/gcn/include gpu/third_party/half/include gpu/third_party/xbyak)
target_compile_options(dcp-tiling-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dcp-tiling-tests PRIVATE dcpgpu)

# Focused tests of the game window's cursor visibility (tools/test.ps1); not part of the default build.
add_executable(dcp-cursor-tests EXCLUDE_FROM_ALL tests/cursor_tests.cpp)
set_target_properties(dcp-cursor-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-cursor-tests PRIVATE gpu/shim)
target_compile_definitions(dcp-cursor-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-cursor-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the windowed-mode size and centring arithmetic; not part of the default build.
add_executable(dcp-window-tests EXCLUDE_FROM_ALL tests/window_fit_tests.cpp)
set_target_properties(dcp-window-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-window-tests PRIVATE src)
target_compile_options(dcp-window-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the swapchain image count, present mode, Gamescope detection and the Gamescope-only flip pacing;
# not part of the default build.
add_executable(dcp-present-tests EXCLUDE_FROM_ALL tests/present_tests.cpp)
set_target_properties(dcp-present-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-present-tests PRIVATE gpu/shim)
target_compile_definitions(dcp-present-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-present-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the experimental AMD water fix switch (issue #32); not part of the default build.
add_executable(dcp-amd-water-tests EXCLUDE_FROM_ALL tests/amd_water_fix_tests.cpp)
set_target_properties(dcp-amd-water-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-amd-water-tests PRIVATE gpu/shim)
target_compile_definitions(dcp-amd-water-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-amd-water-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the Frame rate setting's presentation pacing (tools/test.ps1); not part of the default build.
add_executable(dcp-frame-pacing-tests EXCLUDE_FROM_ALL tests/frame_pacing_tests.cpp)
set_target_properties(dcp-frame-pacing-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-frame-pacing-tests PRIVATE gpu/shim)
target_compile_definitions(dcp-frame-pacing-tests PRIVATE DCP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(dcp-frame-pacing-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the Japanese NPC voice routing (tools/test.ps1); not part of the default build.
add_executable(dcp-voice-tests EXCLUDE_FROM_ALL tests/voice_tests.c src/guest_hook.c src/voice_hooks.c)
target_include_directories(dcp-voice-tests PRIVATE src)
target_compile_options(dcp-voice-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)

# Focused tests of the True Ultrawide hooks (tools/test.ps1); not part of the default build.
add_executable(dcp-ultrawide-tests EXCLUDE_FROM_ALL tests/ultrawide_tests.c src/guest_hook.c src/ultrawide_hooks.c)
target_include_directories(dcp-ultrawide-tests PRIVATE src)
target_compile_options(dcp-ultrawide-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if(NOT WIN32)
    target_link_libraries(dcp-ultrawide-tests PRIVATE m)
endif()

# Focused tests of the True Ultrawide side areas (tools/test.ps1); not part of the default build.
add_executable(dcp-ultrawide-sides-tests EXCLUDE_FROM_ALL tests/ultrawide_sides_tests.cpp)
set_target_properties(dcp-ultrawide-sides-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-ultrawide-sides-tests PRIVATE gpu/shim)
target_compile_options(dcp-ultrawide-sides-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the save-data mounts and the Japanese startup save hook (tools/test.ps1); not part of the default build.
add_executable(dcp-savedata-tests EXCLUDE_FROM_ALL tests/savedata_tests.c src/guest_hook.c src/startup_save_hook.c)
target_include_directories(dcp-savedata-tests PRIVATE src)
target_compile_definitions(dcp-savedata-tests PRIVATE DCP_SAVEDATA_TEST_ROOT="${CMAKE_BINARY_DIR}/savedata-tests")
target_compile_options(dcp-savedata-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_link_libraries(dcp-savedata-tests PRIVATE -pthread)
else()
    target_link_libraries(dcp-savedata-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

if (NOT WIN32)
    # Focused tests of the memory and fiber layers (tools/build.sh --test); not part of the default build.
    add_executable(dcp-runtime-tests EXCLUDE_FROM_ALL tests/runtime_tests.c src/runtime_memory.c src/runtime_fiber.c src/runtime_thread.c)
    target_include_directories(dcp-runtime-tests PRIVATE src)
    target_compile_options(dcp-runtime-tests PRIVATE -Wall -Wextra -Werror -fno-omit-frame-pointer)
    target_link_libraries(dcp-runtime-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
