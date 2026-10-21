# Video core: shadPS4's Liverpool/Vulkan renderer, shader recompiler, GnmDriver, VideoOut and kernel
# event queues, joined to the C loader through shim/dcpgpu.cpp.
cmake_minimum_required(VERSION 3.24)
project(dcpgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Frame pointers keep the loader's fault reports walkable.
add_compile_options(-fno-omit-frame-pointer)

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED CONFIG)
find_package(tsl-robin-map REQUIRED)
find_package(Zydis REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
find_package(SDL3 REQUIRED CONFIG)
find_package(PNG REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

# MSYS2 has no magic_enum, miniz or VMA CMake packages: the fetched sources and header are used.
add_library(magic_enum INTERFACE)
target_include_directories(magic_enum INTERFACE third_party/magic_enum/include third_party/magic_enum/include/magic_enum)
find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
add_library(VulkanMemoryAllocator INTERFACE)
target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# Host shaders -> headers (shadPS4's host_shaders scheme).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${SHADER_SRC}/*.glsl ${SHADER_SRC}/fsr/*.h ${SHADER_SRC}/smaa/*.hlsl)
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} ${SHADER_INCLUDES} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(smaa.vert smaa_edges_vert SMAA_PASS=0)
add_host_shader(smaa.frag smaa_edges_frag SMAA_PASS=0)
add_host_shader(smaa.vert smaa_weights_vert SMAA_PASS=1)
add_host_shader(smaa.frag smaa_weights_frag SMAA_PASS=1)
add_host_shader(smaa.vert smaa_blend_vert SMAA_PASS=2)
add_host_shader(smaa.frag smaa_blend_frag SMAA_PASS=2)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Static: the runtime_* symbols it calls are defined by the loader executable.
add_library(dcpgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
add_dependencies(dcpgpu host_shaders)
target_include_directories(dcpgpu PRIVATE shim shadps4 ../src ${SHADER_INCLUDE} third_party/gcn/include
    third_party/half/include third_party/imgui third_party/imgui/backends third_party/xbyak)
target_compile_definitions(dcpgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    DCP_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/dcp_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_compile_options(dcpgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(dcpgpu PUBLIC Vulkan::Headers Vulkan::Vulkan fmt::fmt Boost::headers magic_enum tsl::robin_map
    VulkanMemoryAllocator PkgConfig::XXHASH SDL3::SDL3 PNG::PNG miniz Zydis::Zydis sirit dcp_keys)
if (WIN32)
    # NOMINMAX/WIN32_LEAN_AND_MEAN keep <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(dcpgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(dcpgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/dcp_win_compat.h)
    target_link_libraries(dcpgpu PUBLIC ws2_32 bcrypt ntdll synchronization winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(dcpgpu PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
