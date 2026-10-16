cmake_minimum_required(VERSION 3.24)
project(dragons_crown_pro_pc LANGUAGES C CXX)
set(DCP_PORT_VERSION "0.3.0")

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

if (NOT DCP_PORT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "DCP_PORT_VERSION must start with MAJOR.MINOR.PATCH")
endif()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/dcp_version.h CONTENT "#define DCP_VERSION_NUMBERS \
${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0\n#define DCP_FILE_VERSION_STRING \
\"${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0\"\n#define DCP_VERSION_STRING \"${DCP_PORT_VERSION}\"\n")

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)

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
    src/runtime_mutex.c
    src/runtime_pad.c
    src/runtime_rtc.c
    src/runtime_rwlock.c
    src/runtime_savedata.c
    src/runtime_sema.c
    src/runtime_services.c
    src/runtime_thread.c
    src/runtime_time.c)

if (WIN32)
    enable_language(RC)
    # Relink when the application icon, the manifest or the version changes.
    set_source_files_properties(src/dcp-loader.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/packaging/icons/app-icon.ico;${CMAKE_SOURCE_DIR}/src/dcp-loader.manifest;${CMAKE_BINARY_DIR}/generated/dcp_version.h")
    set(DCP_PLATFORM_SOURCES src/compat_win.c src/runtime_log.c src/dcp-loader.rc)
else()
    set(DCP_PLATFORM_SOURCES src/runtime_log_posix.c)
endif()
add_executable(dcp-loader src/loader.c src/cheat_hooks.cpp src/guest_hook.c src/runtime_cheats.cpp ${DCP_RUNTIME_SOURCES} ${DCP_PLATFORM_SOURCES})
set_target_properties(dcp-loader PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(dcp-loader PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
target_compile_options(dcp-loader PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
target_compile_options(dcp-loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
if (WIN32)
    # Windows 10 1903+ APIs (VirtualAlloc2, MapViewOfFile3).
    target_compile_definitions(dcp-loader PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    # The PS4 user range (64 GiB - 1 TiB) must stay free of host allocations: no high-entropy ASLR.
    # The guest main thread runs on the process stack: 16 MiB, committed (guest code has no stack probes).
    target_link_options(dcp-loader PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    target_link_libraries(dcp-loader PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    # -rdynamic: fault reports name host symbols (dladdr).
    target_link_options(dcp-loader PRIVATE -rdynamic)
    target_link_libraries(dcp-loader PRIVATE dcp_launcher dcpgpu SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

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

if (NOT WIN32)
    # Focused tests of the memory and fiber layers (tools/build.sh --test); not part of the default build.
    add_executable(dcp-runtime-tests EXCLUDE_FROM_ALL tests/runtime_tests.c src/runtime_memory.c src/runtime_fiber.c src/runtime_thread.c)
    target_include_directories(dcp-runtime-tests PRIVATE src)
    target_compile_options(dcp-runtime-tests PRIVATE -Wall -Wextra -Werror -fno-omit-frame-pointer)
    target_link_libraries(dcp-runtime-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
