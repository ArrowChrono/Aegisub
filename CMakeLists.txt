cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0074 NEW)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
include(CheckIncludeFiles)
include(CMakePackageConfigHelpers)

project(Aegisub)

# Windows SDK target version (see sdkddkver.h).
# 0x0A00 = Windows 10. With _WIN32_WINNT >= Win10, sdkddkver.h defaults
# NTDDI_VERSION to the WDK latest, which exposes dwrite_3.h types we use
# (IDWriteFactory5 / IDWriteFontSetBuilder1 need NTDDI_WIN10_RS2+).
# Runtime still QueryInterfaces and falls back when the host lacks those APIs.
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

option(AEGISUB_LUAJIT_SHARED "Build bundled LuaJIT as a shared library on Windows" OFF)
option(WITH_PLUGIN_BRIDGE "Build the optional Plugin Bridge and Automation support" OFF)
option(WITH_DISCORD_PRESENCE "Build Discord Rich Presence support" ON)
if(AEGISUB_LUAJIT_SHARED AND NOT WIN32)
    message(FATAL_ERROR "AEGISUB_LUAJIT_SHARED is currently only supported on Windows")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Explicitly set the build type to Release if no other type is specified
# on the command line.  Without this, cmake defaults to an unoptimized,
# non-debug build, which almost nobody wants.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Initialize Release IPO for all in-tree targets, including static libraries.
option(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE "Enable link-time optimization for Release builds" ON)
if(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _aegisub_ipo_supported OUTPUT _aegisub_ipo_error LANGUAGES C CXX)
    if(NOT _aegisub_ipo_supported)
        message(FATAL_ERROR
            "Release LTO is not supported by this toolchain. "
            "Set -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF to disable it.\n"
            "${_aegisub_ipo_error}")
    endif()
endif()

# ------------------------------------------------------------------
# Code quality: clang-tidy static analysis
# Automatically enabled when clang-tidy is found in PATH.
# Install LLVM/clang-tools and ensure clang-tidy is on PATH.
# ------------------------------------------------------------------
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    if(NOT TARGET clang-tidy-custom-target)
        add_custom_target(clang-tidy-custom-target)
    endif()
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY}"
        --extra-arg=-Wno-unknown-warning-option
    )
    message(STATUS "Found clang-tidy: ${CLANG_TIDY}")
endif()

## Build bundled LuaJIT with aegisub specific patches
add_executable(luajit-minilua vendor/luajit/src/host/minilua.c)
if(NOT WIN32)
    target_link_libraries(luajit-minilua m)
endif()
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
            COMMAND luajit-minilua ../dynasm/dynasm.lua -LN -D WIN -D JIT -D FFI -D P64 -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h" vm_x86.dasc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
        )
    else()
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
            COMMAND luajit-minilua ../dynasm/dynasm.lua -LN -D WIN -D JIT -D FFI -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h" vm_x86.dasc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
        )
    endif()
elseif(APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
            COMMAND luajit-minilua ../dynasm/dynasm.lua -D ENDIAN_LE -D P64 -D FFI -D DUALNUM -D FPU -D HFABI -D NO_UNWIND -D VER=80 -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h" vm_arm64.dasc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
        )
    else()
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
            COMMAND luajit-minilua ../dynasm/dynasm.lua -D P64 -D JIT -D FFI -D FPU -D HFABI -D VER= -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h" vm_x64.dasc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
        )
    endif()
else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
            COMMAND luajit-minilua ../dynasm/dynasm.lua -D P64 -D JIT -D FFI -D FPU -D HFABI -D VER= -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h" vm_x86.dasc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
        )
    else()
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
            COMMAND luajit-minilua ../dynasm/dynasm.lua -D JIT -D FFI -D FPU -D HFABI -D VER= -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h" vm_x86.dasc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
        )
    endif()
endif()

add_executable(luajit-buildvm
    vendor/luajit/src/host/buildvm.c
    vendor/luajit/src/host/buildvm_asm.c
    vendor/luajit/src/host/buildvm_peobj.c
    vendor/luajit/src/host/buildvm_lib.c
    vendor/luajit/src/host/buildvm_fold.c

    "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/buildvm_arch.h"
)
target_compile_definitions(luajit-buildvm PRIVATE LUAJIT_ENABLE_LUA52COMPAT)
target_include_directories(luajit-buildvm PRIVATE vendor/luajit/src "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen")

if(APPLE)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.s"
        COMMAND luajit-buildvm -m machasm -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.s"
    )
elseif(UNIX)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.s"
        COMMAND luajit-buildvm -m elfasm -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.s"
    )
elseif(MSVC)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.obj"
        COMMAND luajit-buildvm -m peobj -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.obj"
    )
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_ffdef.h"
    COMMAND luajit-buildvm -m ffdef -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_ffdef.h" lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_bcdef.h"
    COMMAND luajit-buildvm -m bcdef -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_bcdef.h" lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_folddef.h"
    COMMAND luajit-buildvm -m folddef -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_folddef.h" lj_opt_fold.c
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_recdef.h"
    COMMAND luajit-buildvm -m recdef -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_recdef.h" lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_libdef.h"
    COMMAND luajit-buildvm -m libdef -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_libdef.h" lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/jit/vmdef.lua"
    COMMAND luajit-buildvm -m vmdef -o "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/jit/vmdef.lua" lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/luajit/src
)

set(AEGISUB_LUAJIT_LIBRARY_TYPE STATIC)
if(WIN32 AND AEGISUB_LUAJIT_SHARED)
    set(AEGISUB_LUAJIT_LIBRARY_TYPE SHARED)
endif()

add_library(luajit ${AEGISUB_LUAJIT_LIBRARY_TYPE}
    vendor/luajit/src/lib_base.c
    vendor/luajit/src/lib_math.c
    vendor/luajit/src/lib_bit.c
    vendor/luajit/src/lib_string.c
    vendor/luajit/src/lib_table.c
    vendor/luajit/src/lib_io.c
    vendor/luajit/src/lib_os.c
    vendor/luajit/src/lib_package.c
    vendor/luajit/src/lib_debug.c
    vendor/luajit/src/lib_jit.c
    vendor/luajit/src/lib_ffi.c
    vendor/luajit/src/lj_gc.c
    vendor/luajit/src/lj_err.c
    vendor/luajit/src/lj_char.c
    vendor/luajit/src/lj_bc.c
    vendor/luajit/src/lj_obj.c
    vendor/luajit/src/lj_buf.c
    vendor/luajit/src/lj_str.c
    vendor/luajit/src/lj_tab.c
    vendor/luajit/src/lj_func.c
    vendor/luajit/src/lj_udata.c
    vendor/luajit/src/lj_meta.c
    vendor/luajit/src/lj_debug.c
    vendor/luajit/src/lj_state.c
    vendor/luajit/src/lj_dispatch.c
    vendor/luajit/src/lj_vmevent.c
    vendor/luajit/src/lj_vmmath.c
    vendor/luajit/src/lj_strscan.c
    vendor/luajit/src/lj_strfmt.c
    vendor/luajit/src/lj_strfmt_num.c
    vendor/luajit/src/lj_api.c
    vendor/luajit/src/lj_profile.c
    vendor/luajit/src/lj_lex.c
    vendor/luajit/src/lj_parse.c
    vendor/luajit/src/lj_bcread.c
    vendor/luajit/src/lj_bcwrite.c
    vendor/luajit/src/lj_load.c
    vendor/luajit/src/lj_ir.c
    vendor/luajit/src/lj_opt_mem.c
    vendor/luajit/src/lj_opt_fold.c
    vendor/luajit/src/lj_opt_narrow.c
    vendor/luajit/src/lj_opt_dce.c
    vendor/luajit/src/lj_opt_loop.c
    vendor/luajit/src/lj_opt_split.c
    vendor/luajit/src/lj_opt_sink.c
    vendor/luajit/src/lj_mcode.c
    vendor/luajit/src/lj_snap.c
    vendor/luajit/src/lj_record.c
    vendor/luajit/src/lj_crecord.c
    vendor/luajit/src/lj_ffrecord.c
    vendor/luajit/src/lj_asm.c
    vendor/luajit/src/lj_trace.c
    vendor/luajit/src/lj_gdbjit.c
    vendor/luajit/src/lj_ctype.c
    vendor/luajit/src/lj_cdata.c
    vendor/luajit/src/lj_cconv.c
    vendor/luajit/src/lj_ccall.c
    vendor/luajit/src/lj_ccallback.c
    vendor/luajit/src/lj_carith.c
    vendor/luajit/src/lj_clib.c
    vendor/luajit/src/lj_cparse.c
    vendor/luajit/src/lj_lib.c
    vendor/luajit/src/lj_alloc.c
    vendor/luajit/src/lib_aux.c
    vendor/luajit/src/lib_init.c

    "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_ffdef.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_bcdef.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_folddef.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_recdef.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen/lj_libdef.h"
    # "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/jit/vmdef.lua"
)
target_compile_definitions(luajit PRIVATE LUAJIT_ENABLE_LUA52COMPAT)
target_include_directories(luajit
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/gen"
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/luajit/src>"
)
if(WIN32 AND AEGISUB_LUAJIT_SHARED)
    target_compile_definitions(luajit PRIVATE LUA_BUILD_AS_DLL)
    set_target_properties(luajit PROPERTIES OUTPUT_NAME lua51)
endif()
if(WIN32)
    target_sources(luajit PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.obj")
else()
    enable_language(ASM)
    target_sources(luajit PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vendor/luajit/src/lj_vm.s")
    target_link_libraries(luajit ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    if(NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
        target_compile_definitions(luajit-buildvm PRIVATE LUAJIT_ENABLE_GC64)
        target_compile_definitions(luajit PRIVATE LUAJIT_ENABLE_GC64)
    endif()
endif()

add_subdirectory(vendor/luabins)
option(LUA_WITH_LUASOCKET "Enable LuaSocket support" OFF)
set(AEGISUB_LUASOCKET_ROOT "${PROJECT_SOURCE_DIR}/vendor/luasocket" CACHE PATH "Path to a LuaSocket source checkout")

if(LUA_WITH_LUASOCKET)
    include(LuaSocket)
    aegisub_add_luasocket_target()
endif()

## libaegisub
add_library(libaegisub STATIC
    libaegisub/common/parser.cpp
    libaegisub/ass/drawing.cpp
    libaegisub/ass/drawing_backend.cpp
    libaegisub/ass/drawing_emit.cpp
    libaegisub/ass/drawing_geom.cpp
    libaegisub/ass/drawing_shape.cpp
    libaegisub/ass/dialogue_parser.cpp
    libaegisub/ass/time.cpp
    libaegisub/ass/uuencode.cpp
    libaegisub/audio/provider.cpp
    libaegisub/audio/provider_convert.cpp
    libaegisub/audio/provider_dummy.cpp
    libaegisub/audio/provider_hd.cpp
    libaegisub/audio/provider_lock.cpp
    libaegisub/audio/provider_pcm.cpp
    libaegisub/audio/provider_ram.cpp
    libaegisub/common/cajun/elements.cpp
    libaegisub/common/cajun/reader.cpp
    libaegisub/common/cajun/writer.cpp
    libaegisub/lua/modules/lfs.cpp
    libaegisub/lua/modules/re.cpp
    libaegisub/lua/modules/unicode.cpp
    libaegisub/lua/modules/lpeg.c
    libaegisub/lua/modules.cpp
    libaegisub/lua/modules/drawing.cpp
    libaegisub/lua/script_reader.cpp
    libaegisub/lua/utils.cpp
    libaegisub/common/calltip_provider.cpp
    libaegisub/common/character_count.cpp
    libaegisub/common/charset.cpp
    libaegisub/common/charset_6937.cpp
    libaegisub/common/charset_conv.cpp
    libaegisub/common/color.cpp
    libaegisub/common/file_mapping.cpp
    libaegisub/common/format.cpp
    libaegisub/common/fs.cpp
    libaegisub/common/native_library.cpp
    libaegisub/common/hotkey.cpp
    libaegisub/common/io.cpp
    libaegisub/common/json.cpp
    libaegisub/common/kana_table.cpp
    libaegisub/common/karaoke_matcher.cpp
    libaegisub/common/keyframe.cpp
    libaegisub/common/line_iterator.cpp
    libaegisub/common/log.cpp
    libaegisub/common/mru.cpp
    libaegisub/common/option.cpp
    libaegisub/common/option_value.cpp
    libaegisub/common/path.cpp
    libaegisub/common/string_utils.cpp
    libaegisub/common/thesaurus.cpp
    libaegisub/common/util.cpp
    libaegisub/common/vfr.cpp
    libaegisub/common/ycbcr_conv.cpp
    libaegisub/common/dispatch.cpp
)
target_compile_features(libaegisub PUBLIC cxx_std_20)
if(UNIX)
    target_sources(libaegisub PRIVATE
        libaegisub/unix/access.cpp
        libaegisub/unix/fs.cpp
        libaegisub/unix/log.cpp
        libaegisub/unix/path.cpp
        libaegisub/unix/util.cpp
    )
    set_property(
        SOURCE libaegisub/unix/path.cpp
        PROPERTY COMPILE_DEFINITIONS
            P_DATA="${CMAKE_INSTALL_PREFIX}/share/aegisub/"
    )
elseif(WIN32)
    target_sources(libaegisub PRIVATE
        libaegisub/windows/access.cpp
        libaegisub/windows/charset_conv_win.cpp
        libaegisub/windows/fs.cpp
        libaegisub/windows/lagi_pre.cpp
        libaegisub/windows/log_win.cpp
        libaegisub/windows/path_win.cpp
        libaegisub/windows/util_win.cpp
    )
endif()
if(APPLE)
    target_sources(libaegisub PRIVATE
        libaegisub/osx/dispatch.mm
        libaegisub/osx/spellchecker.mm
        libaegisub/osx/util.mm
    )
endif()
set_target_properties(libaegisub PROPERTIES PREFIX "")
target_compile_definitions(libaegisub PRIVATE CMAKE_BUILD)
if(WIN32 AND AEGISUB_LUAJIT_SHARED)
    target_compile_definitions(libaegisub PRIVATE LUA_BUILD_AS_DLL)
endif()
target_include_directories(libaegisub PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libaegisub/include>"
)
if(APPLE)
    # CMake on macOS currently does not support pch with objc, so use the old way
    target_compile_options(libaegisub PRIVATE -include "${PROJECT_SOURCE_DIR}/libaegisub/lagi_pre.h")
else()
    target_precompile_headers(libaegisub PRIVATE "libaegisub/lagi_pre.h")
endif()
target_link_libraries(libaegisub PRIVATE luajit luabins)
if(LUA_WITH_LUASOCKET)
    target_compile_definitions(libaegisub PRIVATE LUA_WITH_LUASOCKET)
    target_link_libraries(libaegisub PRIVATE luasocket)
endif()

# --- Shared dependency discovery (consumed by both aegisub_core and Aegisub) ---
# These find_package calls and the highway helper are hoisted here so they are
# available before aegisub_core is defined below, and remain valid for Aegisub.
find_package(ass REQUIRED)
message(STATUS "Found libass headers: ${ass_INCLUDE_DIR}")
if(ass_RUNTIME_LIBRARY)
    message(STATUS "Found libass runtime for staging: ${ass_RUNTIME_LIBRARY}")
else()
    message(STATUS "libass runtime not provided; the provider will be unavailable unless a runtime is installed at deployment time")
endif()
find_package(Boost CONFIG REQUIRED COMPONENTS container locale regex spirit)
find_package(hwy REQUIRED CONFIG)
# Boost::regex needs ICU on Boost < 1.76.0 without the dedicated regex_icu target.
if(TARGET Boost::regex_icu)
    set(AEGISUB_REGEX_TARGET "Boost::regex_icu")
else()
    set(AEGISUB_REGEX_TARGET "Boost::regex")
    find_package(ICU REQUIRED uc dt in)
endif()
# ICU::uc is needed unconditionally (libaegisub charset/unicode, GUI locale).
find_package(ICU REQUIRED uc)
find_package(ZLIB REQUIRED)
find_package(Iconv REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
option(WITH_LIBPLACEBO "Enable optional runtime-loaded libplacebo renderer support" ON)
set(HAVE_LIBPLACEBO OFF)
if(WITH_LIBPLACEBO)
    find_package(LibPlacebo QUIET)
    if(NOT LibPlacebo_FOUND)
        message(FATAL_ERROR
            "WITH_LIBPLACEBO is ON, but libplacebo headers were not found.\n"
            "Install libplacebo development headers or pass -DLibPlacebo_INCLUDE_DIR=/path/to/include.\n"
            "Set -DWITH_LIBPLACEBO=OFF to configure without libplacebo."
        )
    endif()
    set(HAVE_LIBPLACEBO ON)
    message(STATUS "Found libplacebo headers: ${LibPlacebo_INCLUDE_DIR}")
endif()
option(WITH_AVISYNTH "Enable AviSynth support" ON)
if(WITH_AVISYNTH)
    find_package(AviSynth QUIET)
    if(NOT AviSynth_FOUND)
        message(FATAL_ERROR
            "WITH_AVISYNTH is ON, but AviSynth headers were not found.\n"
            "Install AviSynth development headers or pass -DAviSynth_INCLUDE_DIR=/path/to/include.\n"
            "Set -DWITH_AVISYNTH=OFF to configure without AviSynth."
        )
    endif()
    set(HAVE_AVISYNTH ON)
endif()
option(WITH_CSRI "Enable CSRI support" ON)
option(WITH_DRAWING_SKIA "Enable Skia-backed Lua drawing path operations" OFF)
option(WITH_SKIA_VIDEO_TOOLS "Build the opt-in Skia video tools overlay" OFF)
option(WITH_SKIA_AUDIO_DISPLAY "Build the opt-in full Skia audio display" OFF)
option(WITH_SKIA_SUBTITLE_GRID "Build the opt-in Skia CPU subtitle grid renderer" OFF)
option(WITH_SKIA_SOFTWARE_VIDEO "Build the Skia software video presenter (not implemented)" OFF)

foreach(_aegisub_unimplemented_skia_feature IN ITEMS
    WITH_SKIA_SOFTWARE_VIDEO)
    if(${_aegisub_unimplemented_skia_feature})
        message(FATAL_ERROR
            "${_aegisub_unimplemented_skia_feature} is reserved for a later isolated phase and is not implemented in P0."
        )
    endif()
endforeach()

if(WITH_SKIA_VIDEO_TOOLS OR WITH_SKIA_AUDIO_DISPLAY OR WITH_SKIA_SUBTITLE_GRID OR WITH_DRAWING_SKIA)
    # unofficial-skia resolves a few Windows system import libraries while its
    # package config is loaded. Seed those lookups before find_package so a
    # fresh configure does not silently omit them.
    if(WIN32)
        function(aegisub_seed_skia_windows_sdk_import lib_name cache_var)
            if(DEFINED ${cache_var} AND EXISTS "${${cache_var}}")
                return()
            endif()

            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_aegisub_sdk_arch "x64")
            else()
                set(_aegisub_sdk_arch "x86")
            endif()

            file(GLOB _aegisub_sdk_um_dirs LIST_DIRECTORIES true
                "C:/Program Files (x86)/Windows Kits/10/Lib/*/um/${_aegisub_sdk_arch}")
            list(SORT _aegisub_sdk_um_dirs COMPARE NATURAL ORDER DESCENDING)

            foreach(_aegisub_sdk_um_dir IN LISTS _aegisub_sdk_um_dirs)
                unset(_aegisub_sdk_lib CACHE)
                unset(_aegisub_sdk_lib)
                find_library(_aegisub_sdk_lib
                    NAMES "${lib_name}"
                    NAMES_PER_DIR
                    PATHS "${_aegisub_sdk_um_dir}"
                    NO_DEFAULT_PATH)
                if(_aegisub_sdk_lib)
                    set(${cache_var} "${_aegisub_sdk_lib}" CACHE FILEPATH
                        "Windows SDK import preseeded for unofficial-skia" FORCE)
                    break()
                endif()
            endforeach()
        endfunction()

        foreach(_aegisub_cfg RELEASE DEBUG)
            aegisub_seed_skia_windows_sdk_import("FontSub.lib" "z_vcpkg_skia_link_libs_FontSub_lib_${_aegisub_cfg}")
            aegisub_seed_skia_windows_sdk_import("Usp10.lib" "z_vcpkg_skia_link_libs_Usp10_lib_${_aegisub_cfg}")
        endforeach()
    endif()

    find_package(unofficial-skia CONFIG QUIET)
endif()
if(WITH_SKIA_SUBTITLE_GRID)
    if(NOT TARGET unofficial::skia::skia)
        message(FATAL_ERROR
            "WITH_SKIA_SUBTITLE_GRID is ON, but the Skia package was not found.\n"
            "Configure with -DVCPKG_MANIFEST_FEATURES=skia-grid (or skia when a GPU Skia "
            "feature is also enabled), or disable the subtitle grid feature."
        )
    endif()
    if(NOT WIN32 AND NOT TARGET unofficial::skia::modules::skshaper)
        message(FATAL_ERROR
            "WITH_SKIA_SUBTITLE_GRID requires SkShaper on macOS/Linux, but the package target is unavailable."
        )
    endif()
    if(NOT WITH_SKIA_VIDEO_TOOLS AND NOT WITH_SKIA_AUDIO_DISPLAY)
        get_target_property(_aegisub_grid_skia_compile_definitions
            unofficial::skia::skia INTERFACE_COMPILE_DEFINITIONS)
        if("${_aegisub_grid_skia_compile_definitions}" MATCHES "(^|;)SK_GL(;|$)")
            message(FATAL_ERROR
                "A subtitle-grid-only build selected a GL-enabled Skia package. Create a fresh "
                "build directory and configure with -DVCPKG_MANIFEST_FEATURES=skia-grid, or "
                "enable the Audio/Video Skia feature which owns the GPU dependency."
            )
        endif()
        unset(_aegisub_grid_skia_compile_definitions)
    endif()
endif()
option(WITH_FFTW3 "Enable app-local runtime loading of FFTW3 for spectrum FFT support" ON)
find_package(pffft CONFIG QUIET)
option(WITH_PFFFT "Enable PFFFT spectrum FFT support" ${pffft_FOUND})

add_library(aegisub_core_features INTERFACE)
add_library(aegisub_features INTERFACE)
target_link_libraries(aegisub_features INTERFACE aegisub_core_features)
if(WITH_LIBPLACEBO AND HAVE_LIBPLACEBO)
    find_package(OpenGL REQUIRED)
    target_compile_definitions(aegisub_core_features INTERFACE WITH_LIBPLACEBO WITH_LIBPLACEBO_RUNTIME_LOADING)
    target_include_directories(aegisub_core_features INTERFACE
        "$<BUILD_INTERFACE:${LibPlacebo_INCLUDE_DIR}>"
    )
endif()
if(WITH_CSRI)
    target_compile_definitions(aegisub_core_features INTERFACE WITH_CSRI)
endif()
if(WITH_SKIA_VIDEO_TOOLS OR WITH_SKIA_AUDIO_DISPLAY)
    find_package(OpenGL REQUIRED)
    if(NOT TARGET unofficial::skia::skia)
        message(FATAL_ERROR
            "A GUI Skia feature is ON, but the Skia vcpkg package was not found.\n"
            "Configure with -DVCPKG_MANIFEST_FEATURES=skia or disable the GUI Skia feature."
        )
    endif()
    get_target_property(_aegisub_skia_compile_definitions
        unofficial::skia::skia INTERFACE_COMPILE_DEFINITIONS)
    if(NOT "${_aegisub_skia_compile_definitions}" MATCHES "(^|;)SK_GL(;|$)")
        message(FATAL_ERROR
            "GUI Skia requires the Skia gl feature, but the selected package does not "
            "expose SK_GL. Create a fresh build directory (or remove the old "
            "core-only Skia package) and configure with "
            "-DVCPKG_MANIFEST_FEATURES=skia (and VCPKG_MANIFEST_INSTALL=ON), or "
            "disable the GUI Skia feature."
        )
    endif()
    unset(_aegisub_skia_compile_definitions)
endif()
if(WITH_FFTW3)
    target_compile_definitions(aegisub_core_features INTERFACE WITH_FFTW3)
endif()
if(WITH_PFFFT)
    find_package(pffft CONFIG REQUIRED)
    target_compile_definitions(aegisub_core_features INTERFACE WITH_PFFFT)
    target_link_libraries(aegisub_core_features INTERFACE pffft::pffft)
endif()

function(aegisub_enable_highway target_name)
    target_compile_definitions(${target_name} PRIVATE AEGISUB_WITH_HIGHWAY=1)
    target_link_libraries(${target_name} PRIVATE hwy::hwy)
endfunction()

# libaegisub's own codec deps (Boost/regex/ICU/Iconv) -- linked here, after the
# shared find_package calls above, so the imported targets exist.
target_link_libraries(libaegisub PRIVATE "Boost::locale")
target_link_libraries(libaegisub PUBLIC "Boost::headers" "Boost::container")
target_link_libraries(libaegisub PRIVATE ${AEGISUB_REGEX_TARGET})
if(NOT TARGET Boost::regex_icu)
    target_link_libraries(libaegisub PRIVATE "ICU::uc" "ICU::dt" "ICU::in")
endif()
target_link_libraries(libaegisub PRIVATE "Iconv::Iconv")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.cpp" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.h"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/src/libresrc/manifest.respack"
        "${PROJECT_SOURCE_DIR}/src/libresrc/default_config.json"
        "${PROJECT_SOURCE_DIR}/src/libresrc/default_hotkey.json"
        "${PROJECT_SOURCE_DIR}/src/libresrc/default_menu.json"
        "${PROJECT_SOURCE_DIR}/src/libresrc/default_mru.json"
        "${PROJECT_SOURCE_DIR}/src/libresrc/default_toolbar.json"
        "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config_platform.json"
        "${PROJECT_SOURCE_DIR}/tools/respack.lua"
    COMMAND luajit-minilua "${PROJECT_SOURCE_DIR}/tools/respack.lua" manifest.respack default_config.cpp default_config.h
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc"
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.cpp" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.h"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/src/bitmaps/manifest.respack"
        "${PROJECT_SOURCE_DIR}/tools/respack.lua"
    COMMAND luajit-minilua ../../tools/respack.lua manifest.respack "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.cpp" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.h"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/src/bitmaps
)

# Visual Studio can evaluate C++ compilation before custom commands in the
# same project have updated generated headers. Generate resources in a
# dependency target so header timestamps are current before compile checks.
add_custom_target(aegisub_generated_resources
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.h"
        "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.h"
)

add_subdirectory("vendor/csri")
target_sources(csri PRIVATE src/csri_native_library_bridge.cpp)
target_include_directories(csri PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libaegisub/include")
target_compile_features(csri PRIVATE cxx_std_20)
if(WITH_CSRI)
    target_link_libraries(aegisub_core_features INTERFACE csri)
endif()

set(AEGISUB_SHARED_EXE_ENTRY_SOURCES
    src/app_entry.cpp
)

set(AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES
    src/app_runtime_facilities.cpp
)

set(AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES
    ${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}
    src/app_runtime_init.cpp
)

set(AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES
    src/app_process_config.cpp
    src/app_runtime.cpp
    src/automation_runtime_profile.cpp
    ${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}
)

set(AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES
    src/locale_pick.cpp
)

set(AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES
    src/aegisublocale.cpp
    ${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}
)

set(AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES
    src/gui_wx_dispatch_event.cpp
    src/gui_wx_runtime_entry_host.cpp
    src/gui_wx_locale_host.cpp
    src/gui_wx_runtime_host.cpp
    src/gui_wx_subtitle_format_host.cpp
)

set(AEGISUB_GUI_BOOTSTRAP_WX_HOST_SOURCES
    src/gui_wx_bootstrap_ui_host.cpp
)

set(AEGISUB_SHARED_HEADLESS_ENTRY_BOOTSTRAP_SOURCES
    ${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}
    ${AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES}
    src/app_launch_plan.cpp
    src/automation_command_executor.cpp
    src/automation_process_supervisor.cpp
    src/automation_scenario.cpp
    src/automation_scenario_runner.cpp
    src/fontcollector_cli_encoding.cpp
    src/headless_service_output.cpp
    src/headless_process_entry.cpp
    src/headless_runtime_bootstrap.cpp
)

set(AEGISUB_SHARED_SELECTION_REQUEST_SOURCES
    src/charset_choice.cpp
    src/locale_choice.cpp
    src/single_choice_layout.cpp
    src/subtitle_fps_choice.cpp
    src/track_choice.cpp
)

set(AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES
    src/ass_info_service.cpp
    src/media_inspect_service.cpp
    src/trace_inspect_service.cpp
    src/trace_summary_service.cpp
)

set(AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES
    src/playback_query_service.cpp
    src/project_open_service.cpp
)

set(AEGISUB_SHARED_PROVIDER_TRACK_SELECTION_SOURCES
    src/ffmpegsource_common.cpp
)

set(AEGISUB_SHARED_MKV_SUBTITLE_COMMON_SOURCES
    src/mkv_wrap_common.cpp
)

set(AEGISUB_SHARED_MKV_SUBTITLE_LEGACY_BACKEND_SOURCES
    src/MatroskaParser.c
    src/mkv_wrap.cpp
)

set(AEGISUB_SHARED_MKV_SUBTITLE_LIBMATROSKA_BACKEND_SOURCES
    src/mkv_wrap_libmatroska.cpp
)

set(AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_HOST_SOURCES
    src/headless_playback_session_host.cpp
    src/playback_probe_timer.cpp
    src/playback_session_timer.cpp
    src/project_query_service.cpp
)

set(AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES
    ${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_HOST_SOURCES}
    ${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}
)

set(AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES
    src/automation_session_service.cpp
    src/playback_probe_service.cpp
    src/playback_session_service.cpp
    src/project_session_service.cpp
)

set(AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SOURCES
    ${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES}
    ${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}
)

## aegisub_core
#
# wx-free (in intent) static library holding the framework-agnostic core:
# ASS data model, controllers, services, providers, automation host, and the
# presentation/grid_core subdirs. Sources are migrated out of the
# Aegisub executable target so they can be reused by the future C ABI facade
# (and any non-wx host). The PCH is lagi_pre.h (not agi_pre.h) so that any
# residual wx dependency that agi_pre.h used to mask surfaces as a compile
# error -- this is the "debt list" the migration works through.
set(AEGISUB_CORE_SOURCES
    # Translation service (framework-agnostic i18n interface)
    src/app_process_config.cpp
    src/context_core_session.cpp
    src/core_api_facade.cpp
    src/core_host_context.cpp
    src/ebu_export_settings.cpp
    src/numeric_utils.cpp
    src/perspective_ass_bounds.cpp
    src/perspective_apply_plan.cpp
    src/perspective_ass_state.cpp
    src/perspective_forward.cpp
    src/perspective_quad_geometry.cpp
    src/perspective_quad_edit_state.cpp
    src/perspective_tag_solver.cpp
    # Motion tracking and ASS geometry output
    src/motion_track/apply_plan.cpp
    src/motion_track/apply_source.cpp
    src/motion_track/compact_geometry.cpp
    src/motion_track/debug_bundle.cpp
    src/motion_track/geometry_apply.cpp
    src/motion_track/geometry_tags.cpp
    src/motion_track/planar_backend.cpp
    src/motion_track/gray_convert.cpp
    src/motion_track/ncc.cpp
    src/motion_track/raw_batch_motion_frame_reader.cpp
    src/motion_track/session.cpp
    src/motion_track/similarity_backend.cpp
    src/motion_track/synthetic_frame_reader.cpp
    src/motion_track/translation_backend.cpp
    src/translation_service.cpp
    # ASS data model
    src/ass_attachment.cpp
    src/ass_dialogue.cpp
    src/ass_entry.cpp
    src/ass_fixstyle_core.cpp
    # ass_export_filter/ass_exporter expose wxWindow*/wxSizer* in their public
    # API and stay in the exe until abstracted (they are the export subsystem's
    # coupling root, pulling in export_fixstyle/export_framerate/auto4_base).
    src/ass_file.cpp
    src/ass_file_app.cpp
    # ass_info_service.cpp stays with the executable while SubtitleFormat
    # registry and media-inspect services are still host-coupled.
    src/ass_io_core.cpp
    src/ass_karaoke.cpp
    src/ass_override.cpp
    src/ass_parser.cpp
    src/ass_style.cpp
    src/ass_style_resolution.cpp
    src/ass_style_storage.cpp
    src/ass_tag_scanner.cpp
    src/ass_time_projection.cpp
    # Subtitle formats
    src/subtitle_format.cpp
    src/subtitle_format_ass.cpp
    src/subtitle_format_ebu3264.cpp
    src/subtitle_format_encore.cpp
    src/subtitle_format_microdvd.cpp
    src/subtitle_format_mkv.cpp
    src/subtitle_format_srt.cpp
    src/subtitle_format_ssa.cpp
    src/subtitle_format_transtation.cpp
    src/subtitle_format_ttxt.cpp
    src/subtitle_format_txt.cpp
    ${AEGISUB_SHARED_SELECTION_REQUEST_SOURCES}
    # GUI-specific subtitle format prompts are registered by the wx host
    # extension as same-name overrides.
    # Export filters / operations
    # NOTE: export_framerate_transform is wx-free (pure math/vfr), but
    # export_fixstyle/export_framerate/ass_export_filter/ass_exporter expose
    # wxWindow* in their public API (ProcessSubs/GetConfigDialogWindow) and are
    # migrated to the exe until that surface is abstracted.
    src/export_framerate_transform.cpp
    # Subtitle edit / grid / timing ops (framework-agnostic)
    src/subtitle_character_markers.cpp
    src/subtitle_edit_ops.cpp
    src/subtitle_editor_ops.cpp
    src/subtitle_grid_ops.cpp
    src/subtitle_grid_folding.cpp
    src/subtitle_grid_selection_policy.cpp
    src/subtitle_timing_ops.cpp
    # Controllers / context / services
    # context.cpp currently also constructs ContextUiState/DialogManager, so it
    # remains with the wx executable until core/UI construction is split.
    src/initial_line_state.cpp
    src/selection_anchor.cpp
    src/selection_controller.cpp
    src/selection_navigation_history.cpp
    # project.h exposes wxString in ShowError's public signature; migrate to
    # the exe until the API is switched to std::string.
    # project_session_ops is a core-owned media/subtitle session policy layer:
    # it communicates through typed callbacks and NotificationSink.
    # search_replace_engine / secondary_subtitle_session / subs_controller /
    # subs_controller_timer still call UI-facing Context or GUI-only features.
    src/project_session_ops.cpp
    src/secondary_subtitle_reload_policy.cpp
    # src/secondary_subtitle_session.cpp  ← moved to exe (GUI-only feature: dual subtitle preview)
    # subs_controller is still the GUI document controller: it calls
    # SubtitleFormat, command dispatch, FrameMain, and wx file watching.
    # src/subs_controller_timer.cpp  ← moved to exe (wx Timer dependency)
    src/ui_services.cpp
    # Video / audio providers (decoding pipeline)
    # Concrete media providers and async playback runtime still call export
    # filters, perf tracing, config-heavy provider managers, and GUI lifecycle
    # hooks. Keep them with the executable until that runtime has typed seams.
    # Audio rendering / analysis / timing
    # NOTE: audio_marker/audio_colorscheme/audio_renderer(+spectrum/waveform)
    # expose wxPen/wxColour/wxDC/wxBitmap/wxArrayString in their public API, and
    # audio_timing.h returns wxString from a pure virtual. The whole audio
    # rendering cluster is coupled through these roots and stays in the exe
    # until the rendering surface is abstracted (mirrors grid_column_painter).
    # The wx-free analysis/source/player pieces below are kept in core.
    src/audio_display_analysis.cpp
    src/audio_display_source.cpp
    src/audio_latest_range_scheduler.cpp
    src/audio_mix_policy.cpp
    # audio_player.cpp's factory table requires the WITH_* audio-backend defines
    # (ALSA/DirectSound/XAudio2/...) which are exe-only; stays in the exe.
    src/audio_spectrum_analysis_cache.cpp
    src/audio_spectrum_fftw3.cpp
    src/audio_waveform_summary_cache.cpp
    src/align_video_fade.cpp
    src/video_color_pick.cpp
    src/video_color_pick_preview.cpp
    src/resample_dialog_policy.cpp
    src/resolution_resampler.cpp
    # Video rendering / session ops
    src/video_aspect_ratio_ops.cpp
    # video_controller still owns playback/UI coordination and AudioController
    # coupling; keep pure video ops below in core.
    # src/video_frame.cpp  ← moved to exe (GetImage only used in GUI export/display)
    src/async_video_provider.cpp
    src/async_video_provider_host.cpp
    src/async_video_trace.cpp
    src/video_navigation_ops.cpp
    # video_property_update and resolution_resampler are core-owned subtitle
    # document transforms; dialogs/controllers only gather user choices.
    src/video_property_update.cpp
    src/video_property_update_policy.cpp
    src/video_property_update_requests.cpp
    src/video_renderer_backend.cpp
    src/video_session_core_ops.cpp
    src/video_session_ops.cpp
    src/video_snapshot_ops.cpp
    src/navigation_preview_policy.cpp
    src/paste_over_policy.cpp
    # Subtitle rendering providers
    # Provider dependency audit:
    # - core-owned now: subtitles_provider.cpp/libass/secondary decoder; dependencies are
    #   wx-free metadata/catalog, options, libass, native_library, dispatch,
    #   SourceFrame/SubtitleOverlay, and BackgroundRunner.
    # - core-owned now: built-in Dummy/YUV4MPEG plus FFMS and LSMAS concrete
    #   audio/video providers;
    #   dependencies are wx-free track-choice/background seams, runtime loading,
    #   optional SceneChange keyframe generation, and index cache.
    # - core-owned now: audio/video provider managers select from core-owned
    #   built-ins/FFMS/LSMAS/AVS plus host-registered providers.
    # - host-owned for now: playback/controllers/render display pieces which
    #   coordinate UI lifetime, audio controller, and visible windows.
    # libass/plugin subtitle providers are wx-free and form the first concrete
    # provider cluster owned by aegisub_core. Built-in Dummy/YUV4MPEG plus
    # FFMS/LSMAS/AVS and provider managers are concrete video/audio provider
    # clusters owned by aegisub_core. AVS still registers at the host edge so
    # app startup controls provider availability and registry finalization.
    src/audio_provider_factory.cpp
    src/pgs_sup_packet_stream.cpp
    src/vobsub_packet_stream.cpp
    src/secondary_subtitle_decoder.cpp
    src/subtitle_overlay_blend.cpp
    src/subtitles_provider.cpp
    src/subtitles_provider_libass.cpp
    src/libass_runtime.cpp
    src/video_provider_manager.cpp
    src/video_provider_dummy.cpp
    src/video_provider_cache.cpp
    src/video_provider_yuv4mpeg.cpp
    src/cache_cleanup.cpp
    src/provider_index_cache.cpp
    src/provider_catalog_builder.cpp
    src/provider_open_policy.cpp
    # NOTE: auto4_base.h / auto4_lua.h expose wxWindow*/wxDialog*/wxString in
    # their public/protected API (GenerateConfigDialog, CreateWindow, ShowDialog,
    # ProcessingComplete(wxString)). The automation host subsystem is coupled
    # through these roots and stays in the exe until abstracted. The wx-free
    # automation debug subsystem below is kept in core.
    src/automation/automation_breakpoint_store.cpp
    src/automation/automation_context_snapshot.cpp
    src/automation/automation_debug_adapter.cpp
    src/automation/automation_debug_service.cpp
    src/automation/automation_debug_session.cpp
    # live automation/debug backend still depends on auto4 wx progress/dialog
    # objects; the lower-level debug/session model remains in core.
    src/automation/automation_lua_runtime.cpp
    src/automation/automation_mutation_journal.cpp
    src/automation/automation_runtime_journal.cpp
    src/automation/engine/automation_engine_registry.cpp
    # CSRI bridge (renderer discovery + native library bridge)
    src/csri_renderer_discovery.cpp
    # Fonts (cross-platform core)
    src/font_collector_core.cpp
    src/font_collector_unicode.cpp
    src/ass_font_state.cpp
    src/font_face_selection.cpp
    src/font_family_catalog.cpp
    src/font_family_catalog_source.cpp
    src/font_family_selection_model.cpp
    src/font_family_catalog_cache.cpp
    src/font_variant_policy.cpp
    src/font_variant_audit.cpp
    src/font_variant_resolver.cpp
    src/font_name_normalization.cpp
    src/font_portability_audit.cpp
    src/font_file_lister.cpp
    src/font_file_lister_libass.cpp
    src/font_matching_common.cpp
    src/font_matching_libass.cpp
    # Utility / math / codecs
    # NOTE: spline/spline_curve depend on VisualToolBase (coord translator) and
    # transitively on visual_tool.h which exposes wxString. They stay in the
    # exe until the coord-translator is abstracted (part of the visual tool
    # cluster cleanup).
    src/colorspace.cpp
    src/fft.cpp
    src/string_codec.cpp
    src/text_file_reader.cpp
    src/text_file_writer.cpp
    src/time_display_mode.cpp
    src/threaded_ui_timer.cpp
    src/ui_timer.cpp
    src/vector2d.cpp
    src/version.cpp
    src/watched_file_core.cpp
    # Framework-agnostic subdirs
    src/grid_core/grid_layout.cpp
    src/presentation/subtitle_grid_display.cpp
    src/presentation/subtitle_grid_diff.cpp
    src/presentation/subtitle_grid_projection.cpp
    src/presentation/subtitle_grid_query_service.cpp
    # CLI/media/session services still depend on the concrete Project,
    # AudioController, SubtitleFormat registry, and headless playback host. They
    # stay with the wx exe until those dependencies are represented by wx-free
    # typed interfaces.
    # MKV (common; backends selected below)
    ${AEGISUB_SHARED_MKV_SUBTITLE_COMMON_SOURCES}
)

add_library(aegisub_core STATIC ${AEGISUB_CORE_SOURCES})
add_dependencies(aegisub_core aegisub_generated_resources)
target_sources(aegisub_core PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.h"
    "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.h"
)

# CSRI support - subtitles_provider_csri.cpp has no wx dependencies
if(WITH_CSRI)
    target_sources(aegisub_core PRIVATE src/subtitles_provider_csri.cpp)
endif()

if(WITH_AVISYNTH AND HAVE_AVISYNTH)
    target_compile_definitions(aegisub_core PRIVATE WITH_AVISYNTH)
    target_include_directories(aegisub_core PRIVATE "${AviSynth_INCLUDE_DIR}")
    if(WIN32)
        target_compile_definitions(aegisub_core PRIVATE "AVISYNTH_SO=\"AviSynth.dll\"")
        target_link_libraries(aegisub_core PRIVATE Vfw32)
    else()
        target_compile_definitions(aegisub_core PRIVATE "AVISYNTH_SO=\"libavisynth.so\"")
    endif()
    target_sources(aegisub_core PRIVATE
        src/audio_provider_avs.cpp
        src/avisynth_legacy_path.cpp
        src/avisynth_wrap.cpp
        src/video_provider_avs.cpp
    )
endif()

target_compile_features(aegisub_core PUBLIC cxx_std_20)
target_compile_definitions(aegisub_core PRIVATE CMAKE_BUILD)
target_include_directories(aegisub_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>"
)
target_include_directories(aegisub_core PRIVATE vendor)
# version.cpp includes the generated git_version.h; mirror the exe's dependency.
add_dependencies(aegisub_core git_version)
target_include_directories(aegisub_core PRIVATE "build")
# libresrc headers (bitmap.h, default_config.h) are generated and used by automation_session_service.cpp
target_include_directories(aegisub_core PRIVATE "src/libresrc" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc")
set_target_properties(aegisub_core PROPERTIES PREFIX "")
# lagi_pre.h instead of agi_pre.h: surface wx dependencies the PCH used to mask.
if(APPLE)
    target_compile_options(aegisub_core PRIVATE -include "${PROJECT_SOURCE_DIR}/libaegisub/lagi_pre.h")
else()
    target_precompile_headers(aegisub_core PRIVATE "libaegisub/lagi_pre.h")
endif()

set(AEGISUB_CORE_C_API_SOURCES
    src/core_c_api.cpp
)

add_library(aegisub_core_c_api_static STATIC ${AEGISUB_CORE_C_API_SOURCES})
target_compile_features(aegisub_core_c_api_static PRIVATE cxx_std_20)
target_compile_definitions(aegisub_core_c_api_static PRIVATE CMAKE_BUILD)
target_include_directories(aegisub_core_c_api_static
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE src vendor)
target_link_libraries(aegisub_core_c_api_static PUBLIC aegisub_core libaegisub luajit)
set_target_properties(aegisub_core_c_api_static PROPERTIES
    EXPORT_NAME core_c_api_static
    PREFIX ""
    OUTPUT_NAME "aegisub_core_c_api_static"
)
set_target_properties(luajit libaegisub aegisub_core aegisub_core_c_api_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_library(aegisub_core_c_api SHARED ${AEGISUB_CORE_C_API_SOURCES})
target_compile_features(aegisub_core_c_api PRIVATE cxx_std_20)
target_compile_definitions(aegisub_core_c_api PRIVATE CMAKE_BUILD AEGISUB_CORE_BUILD_SHARED)
target_include_directories(aegisub_core_c_api
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE src vendor)
target_link_libraries(aegisub_core_c_api PRIVATE aegisub_core libaegisub luajit)
set_target_properties(aegisub_core_c_api PROPERTIES
    EXPORT_NAME core_c_api
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(UNIX AND NOT APPLE)
    target_link_options(aegisub_core_c_api PRIVATE
        "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/src/aegisub_core_c_api.version"
    )
elseif(APPLE)
    target_link_options(aegisub_core_c_api PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/aegisub_core_c_api.exports"
    )
endif()
# libaegisub is the foundation (signal, dispatch, option, hotkey, fs, ...).
target_link_libraries(aegisub_core PUBLIC libaegisub)
# luajit: the automation debug subsystem (automation_lua_*) includes lua.hpp
# via libaegisub's lua/utils.h. libaegisub links luajit PRIVATE, so core must
# link it directly to inherit the public include path (vendor/luajit/src).
target_link_libraries(aegisub_core PRIVATE luajit)
# libass is loaded at runtime; only public headers are needed at compile time.
# Use BUILD_INTERFACE includes instead of linking ass::headers: aegisub_core is a
# static library, so PRIVATE target deps are still exported as $<LINK_ONLY:...>
# and would force Static Core C API consumers to provide ass::headers.
target_include_directories(aegisub_core PRIVATE
    $<BUILD_INTERFACE:${ass_INCLUDE_DIR}>
)
target_link_libraries(aegisub_core PRIVATE aegisub_core_features "Boost::headers" "Boost::container" "Boost::locale" ${AEGISUB_REGEX_TARGET})
target_link_libraries(aegisub_core PRIVATE pugixml::pugixml)
aegisub_enable_highway(aegisub_core)
target_link_libraries(aegisub_core PRIVATE "ZLIB::ZLIB")
if(WITH_LIBPLACEBO AND HAVE_LIBPLACEBO)
    target_sources(aegisub_core PRIVATE
        src/video_renderer_placebo_runtime.cpp
    )
    set_source_files_properties(
        src/video_renderer_placebo_runtime.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
endif()

# Stage app-local libass runtime next to consumers (Aegisub + core smokes).
# Without an import dependency, vcpkg/CMake will not copy ass.dll automatically.
function(aegisub_stage_libass_runtime target_name)
    if(NOT WIN32 OR NOT ass_RUNTIME_LIBRARY)
        return()
    endif()
    if(NOT TARGET ${target_name})
        return()
    endif()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target_name}>/runtimes"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ass_RUNTIME_LIBRARY}"
            "$<TARGET_FILE_DIR:${target_name}>/runtimes/ass.dll"
        # Prefer the app-local runtimes/ layout only; a leftover root ass.dll
        # can mask missing-runtimes failures and version drift.
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:${target_name}>/ass.dll"
        COMMENT "Staging runtimes/ass.dll for ${target_name}"
        VERBATIM
    )
endfunction()

## Aegisub
add_executable(Aegisub WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.h"
    "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.h"

    src/wx_translation_service.cpp
    src/context.cpp
    src/search_replace_engine.cpp
    src/subtitle_match_report.cpp
    src/subtitle_matcher.cpp
    src/subs_controller.cpp
    src/command/app.cpp
    src/command/audio.cpp
    src/command/automation.cpp
    src/command/command.cpp
    src/command/edit.cpp
    src/command/grid.cpp
    src/command/help.cpp
    src/command/keyframe.cpp
    src/command/recent.cpp
    src/command/subtitle.cpp
    src/command/time.cpp
    src/command/timecode.cpp
    src/command/tool.cpp
    src/command/video.cpp
    src/command/vis_tool.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/bitmap.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.cpp"
    src/libresrc/libresrc.cpp
    src/dialog_about.cpp
    src/dialog_align.cpp
    src/dialog_motion_track.cpp
    src/dialog_attachments.cpp
    src/dialog_automation.cpp
    src/dialog_autosave.cpp
    src/dialog_colorpicker.cpp
    src/dialog_detached_video.cpp
    src/dialog_dummy_video.cpp
    src/dialog_export.cpp
    src/dialog_export_ebu3264.cpp
    src/dialog_font_face.cpp
    src/dialog_font_portability_audit.cpp
    src/dialog_fonts_collector.cpp
    src/dialog_jumpto.cpp
    src/dialog_kara_timing_copy.cpp
    src/dialog_log.cpp
    src/dialog_paste_over.cpp
    src/dialog_progress.cpp
    src/dialog_properties.cpp
    src/dialog_resample.cpp
    src/dialog_search_replace.cpp
    src/dialog_search_results.cpp
    src/dialog_selected_choices.cpp
    src/dialog_selection.cpp
    src/dialog_shift_times.cpp
    src/dialog_spellchecker.cpp
    src/dialog_style_editor.cpp
    src/dialog_style_manager.cpp
    src/dialog_styling_assistant.cpp
    src/dialog_text_import.cpp
    src/dialog_timing_processor.cpp
    src/dialog_translation.cpp
    src/dialog_video_details.cpp
    src/dialog_video_properties.cpp
    src/font_family_catalog_ui.cpp
    src/audio_box.cpp
    src/audio_controller.cpp
    src/wx_audio_controller_power_host.cpp
    src/audio_controller_timer.cpp
    src/audio_display.cpp
    src/audio_karaoke.cpp
    src/audio_player.cpp
    src/base_grid.cpp
    src/charset_detect.cpp
    src/colour_button.cpp
    src/compat.cpp
    src/frame_main.cpp
    src/gl_text.cpp
    src/gl_wrap.cpp
    ${AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES}
    src/grid_column.cpp
    src/grid_column_painter.cpp
    ${AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES}
    ${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}
    ${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}
    ${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_HOST_SOURCES}
    ${AEGISUB_GUI_BOOTSTRAP_WX_HOST_SOURCES}
    src/help_button.cpp
    src/hotkey.cpp
    src/hotkey_data_view_model.cpp
    src/image_position_picker.cpp
    src/main.cpp
    src/menu.cpp
    src/pen.cpp
    src/perf_trace.cpp
    src/persist_location.cpp
    src/preferences.cpp
    src/preferences_base.cpp
    src/secondary_subtitle_session.cpp
    src/secondary_subtitle_session.h
    src/secondary_subtitle_strip.cpp
    src/spellchecker.cpp
    src/subs_edit_box.cpp
    src/subs_edit_ctrl.cpp
    src/subs_preview.cpp
    src/thesaurus.cpp
    src/timeedit_ctrl.cpp
    src/toggle_bitmap.cpp
    src/toolbar.cpp
    src/tooltip_manager.cpp
    src/utils.cpp
    src/validators.cpp
    src/video_overlay_draw_context.h
    src/video_overlay_draw_context_legacy_gl.cpp
    src/video_overlay_draw_context_legacy_gl.h
    src/video_box.cpp
    src/watched_file_wx.cpp
    src/video_controller_timer.cpp
    src/video_controller.cpp
    src/video_color_zoom_preview.cpp
    src/video_display.cpp
    src/video_frame.cpp
    src/video_renderer_factory.cpp
    src/video_renderer_opengl.cpp
    src/video_renderer_opengl_tile.cpp
    src/video_slider.cpp
    src/visual_feature.cpp
    src/visual_guide_controller.cpp
    src/visual_guide_interaction.cpp
    src/visual_guide_model.cpp
    src/visual_guide_overlay.cpp
    src/visual_tool.cpp
    src/visual_tool_clip.cpp
    src/visual_tool_cross.cpp
    src/visual_tool_drag.cpp
    src/visual_tool_measure.cpp
    src/visual_tool_motion_track.cpp
    src/visual_tool_rotatexy.cpp
    src/visual_tool_rotatez.cpp
    src/visual_tool_scale.cpp
    src/visual_tool_vector_clip.cpp
    # src/watched_file.cpp  ← split into watched_file_core.cpp (core) and watched_file_wx.cpp (exe)
    # --- Files moved back from aegisub_core (wx-coupled public APIs) ---
    # Export subsystem (ass_export_filter is the coupling root)
    src/ass_export_filter.cpp
    src/ass_exporter.cpp
    src/export_fixstyle.cpp
    src/export_framerate.cpp
    # Audio rendering cluster (audio_marker / audio_renderer are coupling roots)
    src/audio_colorscheme.cpp
    src/audio_marker.cpp
    src/audio_renderer.cpp
    src/audio_renderer_spectrum.cpp
    src/audio_renderer_waveform.cpp
    src/audio_timing_dialogue.cpp
    src/audio_timing_karaoke.cpp
    # Automation host (auto4_base / auto4_lua are coupling roots)
    src/auto4_base.cpp
    src/auto4_lua.cpp
    src/auto4_lua_assfile.cpp
    src/auto4_lua_dialog.cpp
    src/auto4_lua_progresssink.cpp
    src/automation/automation_live_host.cpp
    src/automation/automation_lua_debug_backend.cpp
    # Controllers with wx in public API
    src/project.cpp
    src/text_selection_controller.cpp
    src/subs_controller_timer.cpp
    # spline cluster (depends on VisualToolBase)
    src/spline.cpp
    src/spline_curve.cpp
    src/fontcollector_subcommand.cpp
    ${AEGISUB_SHARED_EXE_ENTRY_SOURCES}
    ${AEGISUB_SHARED_HEADLESS_ENTRY_BOOTSTRAP_SOURCES}
)
add_dependencies(Aegisub aegisub_generated_resources)
if(WITH_SKIA_VIDEO_TOOLS OR WITH_SKIA_AUDIO_DISPLAY)
    target_sources(Aegisub PRIVATE
        src/skia/skia_gl_contract.cpp
        src/skia/skia_gl_device.cpp
        src/skia_runtime/skia_surface_provider.cpp
    )
    target_compile_definitions(Aegisub PRIVATE
        AEGISUB_WITH_SKIA_GPU_GL=1
    )
    target_link_libraries(Aegisub PRIVATE unofficial::skia::skia OpenGL::GL)
endif()
if(WITH_SKIA_VIDEO_TOOLS)
    target_sources(Aegisub PRIVATE
        src/video_overlay_draw_context_skia.cpp
        src/skia/skia_video_compositor_contract.cpp
        src/skia/skia_video_compositor.cpp
        src/skia/skia_video_overlay_command_buffer.cpp
        src/skia/skia_video_overlay_gl.cpp
        src/skia_runtime/skia_text_layout_cache.cpp
    )
    target_compile_definitions(Aegisub PRIVATE
        AEGISUB_WITH_SKIA_VIDEO_COMPOSITOR=1
        AEGISUB_WITH_SKIA_VIDEO_TOOLS=1
    )
endif()
if(WITH_SKIA_AUDIO_DISPLAY)
    target_sources(Aegisub PRIVATE
        src/skia/audio/skia_audio_content.cpp
        src/skia/audio/skia_audio_content_analysis.cpp
        src/skia/audio/skia_audio_content_worker.cpp
        src/skia/audio/skia_audio_tile_diagnostics.cpp
        src/skia/audio/skia_audio_frame_model.cpp
        src/skia/audio/skia_audio_upload_payload.cpp
        src/skia/audio/skia_audio_display_contract.cpp
        src/skia/audio/skia_audio_display.cpp
        src/skia/audio/skia_audio_presenter.cpp
        src/skia/audio/skia_audio_display_slot.cpp
    )
    target_compile_definitions(Aegisub PRIVATE
        AEGISUB_WITH_SKIA_AUDIO_DISPLAY=1
    )
endif()
if(WITH_SKIA_AUDIO_DISPLAY OR WITH_SKIA_SUBTITLE_GRID)
    target_sources(Aegisub PRIVATE
        src/skia_runtime/platform_font_runtime.cpp
    )
endif()
if(WITH_SKIA_SUBTITLE_GRID)
    target_sources(Aegisub PRIVATE
        src/grid_core/subtitle_grid_renderer_contract.cpp
        src/grid_core/windows_text_raster_policy.cpp
        src/skia_grid_renderer.cpp
        src/skia_grid_text_backend_skshaper.cpp
        src/subtitle_grid_renderer_slot.cpp
    )
    if(WIN32)
        target_sources(Aegisub PRIVATE src/skia_grid_text_backend_dwrite.cpp)
    endif()
    target_compile_definitions(Aegisub PRIVATE
        AEGISUB_WITH_SKIA_SUBTITLE_GRID=1
    )
    target_link_libraries(Aegisub PRIVATE unofficial::skia::skia)
    if(NOT WIN32)
        target_link_libraries(Aegisub PRIVATE unofficial::skia::modules::skshaper)
    endif()
endif()
if(WITH_LIBPLACEBO AND HAVE_LIBPLACEBO)
    target_sources(Aegisub PRIVATE src/video_renderer_placebo_gl.cpp)
    set_source_files_properties(
        src/video_renderer_placebo_gl.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
endif()
set_source_files_properties(libaegisub/common/native_library.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
)
# aegisub_core carries the framework-agnostic core (PUBLIC libaegisub + boost/...).
# libass is runtime-loaded (headers via BUILD_INTERFACE only); see aegisub_stage_libass_runtime.
target_link_libraries(Aegisub PRIVATE ${CMAKE_DL_LIBS} aegisub_core aegisub_features luajit CLI11::CLI11)
if(WITH_DISCORD_PRESENCE)
    add_library(aegisub_discord_presence STATIC
        src/discord_presence_model.cpp
        src/discord_presence_client.cpp
    )
    target_compile_features(aegisub_discord_presence PUBLIC cxx_std_20)
    target_include_directories(aegisub_discord_presence PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(aegisub_discord_presence PUBLIC libaegisub)
    if(WIN32)
        target_link_libraries(aegisub_discord_presence PRIVATE ws2_32)
    endif()
    target_sources(Aegisub PRIVATE src/discord_presence.cpp)
    target_compile_definitions(Aegisub PRIVATE WITH_DISCORD_PRESENCE=1)
    target_link_libraries(Aegisub PRIVATE aegisub_discord_presence)
endif()
aegisub_stage_libass_runtime(Aegisub)
aegisub_stage_libass_runtime(aegisub_core_c_api)
target_compile_definitions(Aegisub PRIVATE CMAKE_BUILD AEGISUB_FONTCOLLECTOR_STATIC)
target_compile_features(Aegisub PRIVATE cxx_std_20)
target_include_directories(Aegisub PRIVATE src)
target_include_directories(Aegisub PRIVATE src/include)
target_include_directories(Aegisub PRIVATE vendor)
option(AEGISUB_MATROSKA_PARSING "Enable MKV subtitle parsing via libebml/libmatroska" ON)
# MKV subtitle import is a core format capability; the selected backend is kept
# in aegisub_core so GUI/headless hosts share the same parser.
if(AEGISUB_MATROSKA_PARSING)
    find_package(EBML REQUIRED CONFIG)
    find_package(Matroska REQUIRED CONFIG)
    target_link_libraries(aegisub_core PRIVATE "EBML::ebml" "Matroska::matroska")
    target_compile_definitions(aegisub_core_features INTERFACE AEGISUB_MATROSKA_PARSING=1)
    target_sources(aegisub_core PRIVATE ${AEGISUB_SHARED_MKV_SUBTITLE_LIBMATROSKA_BACKEND_SOURCES})
else()
    target_compile_definitions(aegisub_core_features INTERFACE AEGISUB_MATROSKA_PARSING=0)
    target_sources(aegisub_core PRIVATE ${AEGISUB_SHARED_MKV_SUBTITLE_LEGACY_BACKEND_SOURCES})
endif()
option(AEGISUB_USE_STRINGZILLA "Use StringZilla in selected string-heavy code paths" ON)
if(AEGISUB_USE_STRINGZILLA)
    find_path(STRINGZILLA_INCLUDE_DIRS NAMES
        "stringzilla/stringzilla.h"
        "stringzilla/stringzilla.hpp"
    )
    if(STRINGZILLA_INCLUDE_DIRS)
        target_include_directories(libaegisub PRIVATE ${STRINGZILLA_INCLUDE_DIRS})
        target_compile_definitions(libaegisub PRIVATE AEGISUB_USE_STRINGZILLA=1)
        target_include_directories(Aegisub PRIVATE ${STRINGZILLA_INCLUDE_DIRS})
        target_compile_definitions(Aegisub PRIVATE AEGISUB_USE_STRINGZILLA=1)
    else()
        message(WARNING "StringZilla headers not found; continuing without StringZilla.")
    endif()
endif()
if(LUA_WITH_LUASOCKET)
    target_compile_definitions(Aegisub PRIVATE LUA_WITH_LUASOCKET)
endif()
if(WIN32 AND AEGISUB_LUAJIT_SHARED)
    target_compile_definitions(Aegisub PRIVATE LUA_BUILD_AS_DLL)
    add_custom_command(TARGET Aegisub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:luajit>
            $<TARGET_FILE_DIR:Aegisub>
    )
endif()
target_include_directories(Aegisub PRIVATE "src/libresrc" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc")
if(APPLE)
    # CMake on macOS currently does not support pch with objc, so use the old way
    target_compile_options(Aegisub PRIVATE -include "${PROJECT_SOURCE_DIR}/src/agi_pre.h")
else()
    target_precompile_headers(Aegisub PRIVATE "src/agi_pre.h")
endif()

if(WIN32)
    add_custom_target(git_version
        COMMAND "${PROJECT_SOURCE_DIR}/build/version.cmd" .
        BYPRODUCTS "${PROJECT_SOURCE_DIR}/build/git_version.h"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        VERBATIM
    )
    add_dependencies(Aegisub git_version)
    target_include_directories(Aegisub PRIVATE "build")
else()
    add_custom_target(git_version build/version.sh .
        BYPRODUCTS "${PROJECT_SOURCE_DIR}/build/git_version.h"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    add_dependencies(Aegisub git_version)
    target_include_directories(Aegisub PRIVATE "build")
endif()

option(WITH_BUILD_CREDIT "Whether show build credit in about dialog")
if(WITH_BUILD_CREDIT)
    set(BUILD_CREDIT "" CACHE STRING "Build credit shown in about dialog")
    target_compile_definitions(Aegisub PRIVATE "BUILD_CREDIT=${BUILD_CREDIT}")
else()
    unset(BUILD_CREDIT CACHE)
endif()

if(MSVC)
    add_definitions("/DNOMINMAX /DINITGUID")
    target_link_libraries(Aegisub LINK_PRIVATE Usp10 dwrite)
    target_link_libraries(Aegisub PRIVATE Psapi)
    #target_sources(Aegisub PRIVATE src/res/res.rc src/res/strings.rc src/crash_writer_minidump.cpp)
    target_sources(Aegisub PRIVATE src/res/res.rc src/res/strings.rc src/crash_writer.cpp src/dpi_aware.manifest)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Aegisub)
elseif(APPLE)
    target_include_directories(Aegisub PRIVATE src)  # For src/retina_helper.h
    target_sources(Aegisub PRIVATE src/crash_writer.cpp src/osx/osx_utils.mm src/osx/retina_helper.mm)
else()
    target_sources(Aegisub PRIVATE src/crash_writer.cpp)
endif()

if(WIN32)
    # Platform: GDI/DirectWrite. Libass matcher: DirectWrite provider. No Fontconfig.
    target_sources(aegisub_core PRIVATE
        src/font_file_lister_gdi.cpp
        src/font_file_lister_dwrite.cpp
        src/skia_runtime/dwrite_runtime.cpp
        src/gdi_font_resolver.cpp
        src/font_family_observe_win.cpp
        src/font_family_derive_win.cpp
        src/font_family_obs_store.cpp
        src/font_family_obs_face_stamp_win.cpp
        src/font_family_obs_repository_win.cpp
        src/font_family_catalog_win.cpp
        src/font_variant_resolver_win.cpp)
    target_link_libraries(aegisub_core PRIVATE Usp10 dwrite Version)
elseif(APPLE)
    # Platform: CoreText always. Libass matcher: optional Fontconfig catalog
    # (there is no CoreText ILibassFontProvider yet). Do not hard-require Fontconfig
    # so CoreText-only builds still work; libass mode errors at runtime if missing.
    target_sources(aegisub_core PRIVATE src/font_file_lister_coretext.mm)
    # The fontcollector C API can be consumed without the wx executable. Keep
    # the frameworks on the core target so both the in-tree facade and installed
    # static consumers receive the CoreText link requirements.
    target_link_libraries(aegisub_core PRIVATE "-framework AppKit" "-framework CoreText")
    find_package(Fontconfig)
    if(Fontconfig_FOUND)
        target_compile_definitions(aegisub_core PRIVATE AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG)
        target_sources(aegisub_core PRIVATE src/font_file_lister_fontconfig.cpp)
        target_link_libraries(aegisub_core PRIVATE Fontconfig::Fontconfig)
    else()
        message(STATUS
            "Fontconfig not found: macOS libass font matcher disabled (platform/CoreText only); "
            "enable the vcpkg manifest feature 'fontconfig' to install it")
    endif()
else()
    # Linux/Unix: Fontconfig is both the platform provider and the libass catalog.
    find_package(Fontconfig REQUIRED)
    target_link_libraries(aegisub_core PRIVATE "Fontconfig::Fontconfig")
    target_sources(aegisub_core PRIVATE src/font_file_lister_fontconfig.cpp)
    target_link_libraries(Aegisub PRIVATE "Fontconfig::Fontconfig")
endif()

# Boost / regex / ICU / Highway / Iconv / ZLIB are pulled in transitively via
# aegisub_core and libaegisub. libass is NOT linked: the core only needs
# compile-time headers (BUILD_INTERFACE) and loads ass.dll / libass at runtime.
# The exe additionally needs Boost::locale (GUI-side wx locale helpers) and ICU::uc.
target_link_libraries(Aegisub PRIVATE "Boost::locale" "ICU::uc")

aegisub_enable_highway(Aegisub)

find_package(OpenGL REQUIRED)
target_link_libraries(Aegisub PRIVATE "OpenGL::GL")
check_include_files(OpenGL/gl.h HAVE_OPENGL_GL_H)
if(HAVE_OPENGL_GL_H)
    target_compile_definitions(Aegisub PRIVATE "HAVE_OPENGL_GL_H")
endif()

# Skia: WITH_DRAWING_SKIA adds a Lua drawing backend to libaegisub. GUI Skia
# features are executable-private and configured independently above.
if(WITH_DRAWING_SKIA)
    if(NOT TARGET unofficial::skia::skia)
        message(FATAL_ERROR
            "WITH_DRAWING_SKIA is ON, but the Skia vcpkg package was not found.\n"
            "Configure with -DVCPKG_MANIFEST_FEATURES=drawing-skia or pass -DWITH_DRAWING_SKIA=OFF."
        )
    endif()
    target_compile_definitions(libaegisub PRIVATE WITH_DRAWING_SKIA)
    target_link_libraries(libaegisub PRIVATE unofficial::skia::skia)
endif()

find_package(wxWidgets REQUIRED adv base core gl propgrid stc)
option(WITH_WXSTC "Enable wxStyledTextCtrl support" ON)
function(aegisub_enable_wx_utf8_guard target_name)
    if(TARGET ${target_name})
        target_compile_definitions(${target_name} PRIVATE "wxNO_IMPLICIT_WXSTRING_ENCODING")
    endif()
endfunction()

aegisub_enable_wx_utf8_guard(Aegisub)
if(WITH_WXSTC)
    target_compile_definitions(Aegisub PRIVATE "WITH_WXSTC")
    target_sources(Aegisub PRIVATE src/subs_edit_ctrl_stc.cpp)
endif()
include(${wxWidgets_USE_FILE})
target_link_libraries(Aegisub LINK_PRIVATE ${wxWidgets_LIBRARIES})

# ZLIB::ZLIB is already available (found in the shared discovery block above);
# the exe pulls it in transitively via aegisub_core too, but link explicitly
# for clarity since several GUI sources use zlib directly.
target_link_libraries(Aegisub PRIVATE "ZLIB::ZLIB")

find_package(ALSA)
option(WITH_ALSA "Enable ALSA support" ${ALSA_FOUND})
if(WITH_ALSA)
    find_package(ALSA REQUIRED)
    target_compile_definitions(Aegisub PRIVATE "WITH_ALSA")
    target_link_libraries(Aegisub PRIVATE "ALSA::ALSA")
    target_sources(Aegisub PRIVATE src/audio_player_alsa.cpp)
endif()

# AviSynth+: option/find_package/HAVE_AVISYNTH live in the shared discovery
# block above. Provider implementation is compiled into aegisub_core; the wx
# host keeps only the explicit registration decision during startup.
if(WITH_AVISYNTH AND HAVE_AVISYNTH)
    target_compile_definitions(Aegisub PRIVATE WITH_AVISYNTH)
    target_include_directories(Aegisub PRIVATE "${AviSynth_INCLUDE_DIR}")
    if(WIN32)
        target_compile_definitions(Aegisub PRIVATE "AVISYNTH_SO=\"AviSynth.dll\"")
    else()
        target_compile_definitions(Aegisub PRIVATE "AVISYNTH_SO=\"libavisynth.so\"")
    endif()
endif()

# CSRI option is defined in the shared discovery block; the provider source is
# compiled into aegisub_core so it is not also added to the executable.

if(WIN32)
    target_link_libraries(Aegisub PRIVATE Wtsapi32)
    option(WITH_DIRECTSOUND "Enable DirectSound support" ON)
    if(WITH_DIRECTSOUND)
        target_compile_definitions(Aegisub PRIVATE "WITH_DIRECTSOUND")
        target_link_libraries(Aegisub LINK_PRIVATE dsound)
        target_sources(Aegisub PRIVATE src/audio_player_dsound.cpp src/audio_player_dsound2.cpp)
    endif()
    option(WITH_XAUDIO2 "Enable XAudio2 support" ON)
    if(WITH_XAUDIO2)
        target_compile_definitions(Aegisub PRIVATE "WITH_XAUDIO2")
        target_sources(Aegisub PRIVATE src/audio_player_xaudio2.cpp)
        option(XAUDIO2_REDIST "Use XAudio redistributable")
        if(XAUDIO2_REDIST)
            find_package(XAudio2redist REQUIRED)
            target_compile_definitions(Aegisub PRIVATE "XAUDIO2_REDIST")
            target_include_directories(Aegisub PRIVATE ${XAudio2redist_INCLUDE_DIRS})
            target_link_libraries(Aegisub PRIVATE ${XAudio2redist_LIBRARIES})
        endif()
    endif()
else()
    set(WITH_DIRECTSOUND OFF)
    set(WITH_XAUDIO2 OFF)
endif()

if(APPLE)
    option(WITH_COREAUDIO "Enable CoreAudio support" ON)
    if(WITH_COREAUDIO)
        target_compile_definitions(Aegisub PRIVATE "WITH_COREAUDIO")
        target_link_libraries(Aegisub PRIVATE "-framework AudioToolbox")
        target_sources(Aegisub PRIVATE src/audio_player_coreaudio.cpp)
    endif()
else()
    set(WITH_COREAUDIO OFF)
endif()

set(HAVE_FFMS2 OFF)
option(WITH_FFMS2 "Enable FFMS2 support" ON)
if(WITH_FFMS2)
    find_package(FFMS2 QUIET)

    if(NOT FFMS2_FOUND)
        message(FATAL_ERROR
            "WITH_FFMS2 is ON, but FFMS2 headers were not found.\n"
            "Install FFMS2 development headers or pass -DFFMS2_INCLUDE_DIR=/path/to/include.\n"
            "Set -DWITH_FFMS2=OFF to configure without FFMS2."
        )
    endif()

    set(HAVE_FFMS2 ON)
    message(STATUS "Found FFMS2 headers: ${FFMS2_INCLUDE_DIR}")
    target_compile_definitions(aegisub_core_features INTERFACE "WITH_FFMS2" "WITH_FFMS2_RUNTIME_LOADING")
    target_include_directories(aegisub_core_features INTERFACE
        "$<BUILD_INTERFACE:${FFMS2_INCLUDE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffms2>"
    )
    target_sources(aegisub_core PRIVATE
        src/audio_provider_ffmpegsource.cpp
        ${AEGISUB_SHARED_PROVIDER_TRACK_SELECTION_SOURCES}
        src/video_provider_ffmpegsource.cpp
    )
elseif(NOT HAVE_FFMS2)
    message(WARNING
        "No supported video/audio reader interface was enabled.\n"
        "You will not be able to open any video or audio files in Aegisub unless you install a supported video/audio provider.\n"
        "You will however still be able to open \"dummy\" video, ie. a blank, virtual video clip with subtitles overlaid.\n"
        "Currently we only support one video/audio provider on non-Windows systems:\n"
        " - FFMS2\n"
        "   * http://github.com/FFMS/ffms2\n"
    )
endif()

set(HAVE_LSMASNATIVE OFF)
option(WITH_LSMASNATIVE "Enable LsmasNative provider support" ON)
if(WITH_LSMASNATIVE)
    set(LSMASNATIVE_GENERATED_API "${CMAKE_CURRENT_SOURCE_DIR}/src/lsmas_native_api.generated.h")
    set(LSMASNATIVE_FUNCTIONS_INC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lsmasnative/lsmas_native_api.functions.inc")
    if(NOT EXISTS "${LSMASNATIVE_GENERATED_API}" OR NOT EXISTS "${LSMASNATIVE_FUNCTIONS_INC}")
        message(FATAL_ERROR
            "WITH_LSMASNATIVE is ON, but generated LsmasNative API files are missing.\n"
            "Run: dotnet vendor/lsmasnative/gen_lsmas_native_api.cs -- --mode used --header /path/to/lsmas_native.h"
        )
    endif()

    set(HAVE_LSMASNATIVE ON)
    target_compile_definitions(aegisub_core_features INTERFACE "WITH_LSMASNATIVE")
    target_include_directories(aegisub_core_features INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/lsmasnative>"
    )
    target_sources(aegisub_core PRIVATE
        src/lsmas_native_runtime.cpp
        src/lsmas_provider_common.cpp
        src/video_provider_lsmasnative.cpp
        src/audio_provider_lsmasnative.cpp
    )
endif()

set(HAVE_SCENECHANGE OFF)
option(WITH_SCENECHANGE "Enable SceneChange NativeAOT runtime support" ON)
if(WITH_SCENECHANGE)
    set(HAVE_SCENECHANGE ON)
    target_compile_definitions(aegisub_core_features INTERFACE "WITH_SCENECHANGE")
    target_sources(aegisub_core PRIVATE
        src/scenechange_native_runtime.cpp
    )
endif()

find_package(Hunspell)
option(WITH_HUNSPELL "Enable Hunspell support" ${HUNSPELL_FOUND})
if(WITH_HUNSPELL)
    find_package(Hunspell REQUIRED)
    target_compile_definitions(Aegisub PRIVATE "WITH_HUNSPELL")
    target_link_libraries(Aegisub PRIVATE "hunspell")
    target_sources(Aegisub PRIVATE src/spellchecker_hunspell.cpp)
endif()

find_package(PulseAudio)
option(WITH_LIBPULSE "Enable PulseAudio support" ${PULSEAUDIO_FOUND})
if(WITH_LIBPULSE)
    find_package(PulseAudio REQUIRED)
    target_compile_definitions(Aegisub PRIVATE "WITH_LIBPULSE")
    target_include_directories(Aegisub PRIVATE ${PULSEAUDIO_INCLUDE_DIR})
    target_link_libraries(Aegisub LINK_PRIVATE ${PULSEAUDIO_LIBRARY})
    target_sources(Aegisub PRIVATE src/audio_player_pulse.cpp)
endif()

find_package(OpenAL)
option(WITH_OPENAL "Enable OpenAL support" ${OPENAL_FOUND})
if(WITH_OPENAL)
    find_package(OpenAL REQUIRED)
    target_compile_definitions(Aegisub PRIVATE "WITH_OPENAL")
    target_include_directories(Aegisub PRIVATE ${OPENAL_INCLUDE_DIR})
    target_link_libraries(Aegisub LINK_PRIVATE ${OPENAL_LIBRARY})
    target_sources(Aegisub PRIVATE src/audio_player_openal.cpp)
endif()

find_package(OSS)
option(WITH_OSS "Enable OSS support" ${OSS_FOUND})
if(WITH_OSS)
    find_package(OSS REQUIRED)
    target_compile_definitions(Aegisub PRIVATE "WITH_OSS")
    target_include_directories(Aegisub PRIVATE ${OSS_INCLUDE_DIRS})
    target_sources(Aegisub PRIVATE src/audio_player_oss.cpp)
endif()

find_package(PortAudio)
option(WITH_PORTAUDIO "Enable PortAudio support" ${PortAudio_FOUND})
if(WITH_PORTAUDIO)
    find_package(PortAudio REQUIRED)
    target_compile_definitions(Aegisub PRIVATE "WITH_PORTAUDIO")
    target_include_directories(Aegisub PRIVATE ${PortAudio_INCLUDE_DIRS})
    target_link_libraries(Aegisub LINK_PRIVATE ${PortAudio_LIBRARIES})
    target_sources(Aegisub PRIVATE src/audio_player_portaudio.cpp)
endif()

option(WITH_STARTUPLOG "Enable startup log")
if(WITH_STARTUPLOG)
    target_compile_definitions(Aegisub PRIVATE "WITH_STARTUPLOG")
endif()

find_package(uchardet)
option(WITH_UCHARDET "Enable uchardet support" ${uchardet_FOUND})
if(WITH_UCHARDET)
    find_package(uchardet REQUIRED)
    target_compile_definitions(libaegisub PRIVATE "WITH_UCHARDET")
    target_link_libraries(libaegisub PRIVATE uchardet::libuchardet)
endif()

option(WITH_UPDATE_CHECKER "Enable update checker")
if(WITH_UPDATE_CHECKER)
    set(UPDATE_CHECKER_SERVER "\"updates.aegisub.org\"" CACHE STRING "Server for the update checker")
    set(UPDATE_CHECKER_BASE_URL "\"/trunk\"" CACHE STRING "Base path for the update checker")
    target_compile_definitions(Aegisub PRIVATE "WITH_UPDATE_CHECKER" "UPDATE_CHECKER_SERVER=${UPDATE_CHECKER_SERVER}" "UPDATE_CHECKER_BASE_URL=${UPDATE_CHECKER_BASE_URL}")
    target_sources(Aegisub PRIVATE src/dialog_version_check.cpp)
endif()

if(WIN32)
    if(WITH_XAUDIO2)
        set(DEFAULT_PLAYER_AUDIO "XAudio2" CACHE STRING "Default audio player")
    elseif(WITH_DIRECTSOUND)
        set(DEFAULT_PLAYER_AUDIO "DirectSound" CACHE STRING "Default audio player")
    else()
        message(SEND_ERROR
            "No supported audio player interface was enabled.\n"
            "If you want audio support in Aegisub you need to enable one of these audio players:\n"
            " - XAudio2\n"
            " - DirectSound\n"
            "\n"
        )
        set(DEFAULT_PLAYER_AUDIO "NONE" CACHE STRING "Default audio player")
    endif()
    configure_file("src/libresrc/default_config_win.json.in" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config_platform.json" @ONLY)
else()
    if(WITH_LIBPULSE)
        set(DEFAULT_PLAYER_AUDIO "PulseAudio" CACHE STRING "Default audio player")
    elseif(WITH_ALSA)
        set(DEFAULT_PLAYER_AUDIO "ALSA" CACHE STRING "Default audio player")
    elseif(WITH_COREAUDIO)
        set(DEFAULT_PLAYER_AUDIO "CoreAudio" CACHE STRING "Default audio player")
    elseif(WITH_OPENAL)
        set(DEFAULT_PLAYER_AUDIO "OpenAL" CACHE STRING "Default audio player")
    elseif(WITH_PORTAUDIO)
        set(DEFAULT_PLAYER_AUDIO "PortAudio" CACHE STRING "Default audio player")
    elseif(WITH_OSS)
        set(DEFAULT_PLAYER_AUDIO "OSS" CACHE STRING "Default audio player")
    else()
        message(SEND_ERROR
            "No supported audio player interface was enabled.\n"
            "If you want audio support in Aegisub you need to install one of these libraries:\n"
            " - PulseAudio\n"
            "   * http://pulseaudio.org/\n"
            " - ALSA (Linux only)\n"
            "   * http://www.alsa-project.org/\n"
            " - PortAudio (version 19 only)\n"
            "   * http://www.portaudio.com/\n"
            "\n"
        )
        set(DEFAULT_PLAYER_AUDIO "NONE" CACHE STRING "Default audio player")
    endif()
    configure_file("src/libresrc/default_config_platform.json.in" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config_platform.json" @ONLY)
endif()
configure_file("src/libresrc/default_config.json" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_config.json" COPYONLY)
configure_file("src/libresrc/default_hotkey.json" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_hotkey.json" COPYONLY)
configure_file("src/libresrc/default_menu.json" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_menu.json" COPYONLY)
configure_file("src/libresrc/default_mru.json" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_mru.json" COPYONLY)
configure_file("src/libresrc/default_toolbar.json" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/default_toolbar.json" COPYONLY)
configure_file("src/libresrc/manifest.respack" "${CMAKE_CURRENT_BINARY_DIR}/src/libresrc/manifest.respack" COPYONLY)

if(NOT WIN32)
    set(AEGISUB_COMMAND "aegisub" CACHE STRING "The executable name of Aegisub")
    set_target_properties(Aegisub PROPERTIES OUTPUT_NAME "${AEGISUB_COMMAND}")
    configure_file("packages/desktop/aegisub.desktop.template.in" "aegisub.desktop" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/aegisub.desktop" DESTINATION "share/applications")
    install(FILES "packages/desktop/16x16.png" DESTINATION "share/icons/hicolor/16x16/apps" RENAME aegisub.png)
    install(FILES "packages/desktop/22x22.png" DESTINATION "share/icons/hicolor/22x22/apps" RENAME aegisub.png)
    install(FILES "packages/desktop/24x24.png" DESTINATION "share/icons/hicolor/24x24/apps" RENAME aegisub.png)
    install(FILES "packages/desktop/32x32.png" DESTINATION "share/icons/hicolor/32x32/apps" RENAME aegisub.png)
    install(FILES "packages/desktop/48x48.png" DESTINATION "share/icons/hicolor/48x48/apps" RENAME aegisub.png)
    install(FILES "packages/desktop/64x64.png" DESTINATION "share/icons/hicolor/64x64/apps" RENAME aegisub.png)
    install(FILES "packages/desktop/scalable.svg" DESTINATION "share/icons/hicolor/scalable/apps" RENAME aegisub.svg)
endif()
install(TARGETS Aegisub DESTINATION bin)

# Font collector C API
#
# The font collector implementation is already part of aegisub_core.  Keep
# this facade in its own targets so the application can link the static form
# without compiling a second copy of the core, while SDK consumers can use a
# shared DLL from the same build. The main project owns both SDK forms so the
# command and external API use one canonical implementation.
add_library(aegisub_fontcollector_api STATIC
    src/fontcollector_api.cpp
)
add_library(Aegisub::fontcollector_static ALIAS aegisub_fontcollector_api)
set_target_properties(aegisub_fontcollector_api PROPERTIES
    EXPORT_NAME fontcollector_static
    PREFIX ""
    OUTPUT_NAME "aegisub_fontcollector_static"
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(aegisub_fontcollector_api PRIVATE cxx_std_20)
target_compile_definitions(aegisub_fontcollector_api
    PUBLIC AEGISUB_FONTCOLLECTOR_STATIC
    PRIVATE CMAKE_BUILD
)
target_include_directories(aegisub_fontcollector_api
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/libaegisub"
)
target_link_libraries(aegisub_fontcollector_api PUBLIC aegisub_core)

add_library(aegisub_fontcollector SHARED
    src/fontcollector_api.cpp
)
add_library(Aegisub::fontcollector ALIAS aegisub_fontcollector)
set_target_properties(aegisub_fontcollector PROPERTIES
    EXPORT_NAME fontcollector
    PREFIX ""
    OUTPUT_NAME "aegisub_fontcollector"
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(UNIX AND NOT APPLE)
    target_link_options(aegisub_fontcollector PRIVATE
        "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/cmake/aegisub_fontcollector.version"
    )
    set_property(TARGET aegisub_fontcollector APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aegisub_fontcollector.version")
elseif(APPLE)
    target_link_options(aegisub_fontcollector PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/aegisub_fontcollector.exports"
    )
    set_property(TARGET aegisub_fontcollector APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aegisub_fontcollector.exports")
endif()
target_compile_features(aegisub_fontcollector PRIVATE cxx_std_20)
target_compile_definitions(aegisub_fontcollector
    PRIVATE CMAKE_BUILD AEGISUB_FONTCOLLECTOR_BUILD
)
target_include_directories(aegisub_fontcollector
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/libaegisub"
)
target_link_libraries(aegisub_fontcollector PRIVATE aegisub_core)
aegisub_stage_libass_runtime(aegisub_fontcollector)

# Keep the GUI/CLI executable self-contained.  The shared target is an SDK
# artifact and is not a runtime dependency of Aegisub.
target_link_libraries(Aegisub PRIVATE aegisub_fontcollector_api)

set(AEGISUB_CORE_API_SHARED_EXPORT_TARGETS
    aegisub_fontcollector
    aegisub_core_c_api
)

set(AEGISUB_CORE_API_STATIC_EXPORT_TARGETS
    aegisub_fontcollector_api
    aegisub_core_c_api_static
    aegisub_core
    libaegisub
    luabins
    aegisub_core_features
)
if(AEGISUB_LUAJIT_SHARED)
    # Shared C API targets retain LuaJIT as an imported runtime dependency.
    # Export it with the shared set so Shared-only package consumers can resolve
    # that target before the optional static export is loaded.
    list(APPEND AEGISUB_CORE_API_SHARED_EXPORT_TARGETS luajit)
else()
    list(APPEND AEGISUB_CORE_API_STATIC_EXPORT_TARGETS luajit)
endif()
if(LUA_WITH_LUASOCKET)
    list(APPEND AEGISUB_CORE_API_STATIC_EXPORT_TARGETS luasocket)
endif()
if(WITH_CSRI)
    list(APPEND AEGISUB_CORE_API_STATIC_EXPORT_TARGETS csri)
endif()

set(AEGISUB_CORE_API_RUNTIME_DEPENDENCY_ARGS)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    set(AEGISUB_CORE_API_RUNTIME_DEPENDENCY_ARGS
        RUNTIME_DEPENDENCY_SET AegisubCoreCAPIRuntimeDeps
    )
endif()
install(TARGETS ${AEGISUB_CORE_API_SHARED_EXPORT_TARGETS}
    EXPORT AegisubCoreCAPISharedTargets
    ${AEGISUB_CORE_API_RUNTIME_DEPENDENCY_ARGS}
    RUNTIME DESTINATION bin COMPONENT CoreAPI
    LIBRARY DESTINATION lib COMPONENT CoreAPI
    ARCHIVE DESTINATION lib COMPONENT CoreAPI
)
install(TARGETS ${AEGISUB_CORE_API_STATIC_EXPORT_TARGETS}
    EXPORT AegisubCoreCAPITargets
    RUNTIME DESTINATION bin COMPONENT CoreAPI
    LIBRARY DESTINATION lib COMPONENT CoreAPI
    ARCHIVE DESTINATION lib COMPONENT CoreAPI
)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    install(RUNTIME_DEPENDENCY_SET AegisubCoreCAPIRuntimeDeps
        PRE_EXCLUDE_REGEXES
            [[api-ms-.*]]
            [[ext-ms-.*]]
        POST_EXCLUDE_REGEXES
            [[.*[\\/]System32[\\/].*]]
            [[.*[\\/]SysWOW64[\\/].*]]
            [[.*[\\/]system32[\\/].*]]
            [[.*[\\/]syswow64[\\/].*]]
        RUNTIME DESTINATION bin
        COMPONENT CoreAPI
    )
endif()
# libass is LoadLibrary'd at runtime and is invisible to RUNTIME_DEPENDENCY_SET.
if(WIN32 AND ass_RUNTIME_LIBRARY)
    install(FILES "${ass_RUNTIME_LIBRARY}"
        DESTINATION bin/runtimes
        COMPONENT CoreAPI
        RENAME ass.dll
    )
endif()
install(FILES src/include/aegisub/core_c_api.h
    DESTINATION include/aegisub
    COMPONENT CoreAPI
)
install(FILES src/include/aegisub/fontcollector/fontcollector.h
    DESTINATION include/aegisub/fontcollector
    COMPONENT CoreAPI
)

set(AEGISUB_CORE_API_NEEDS_SKIA OFF)
if(WITH_DRAWING_SKIA)
    set(AEGISUB_CORE_API_NEEDS_SKIA ON)
endif()

# The static export includes the platform font provider. Linux always links
# Fontconfig; macOS does so only when its optional provider was found. Shared
# consumers do not need this dependency because the provider is inside the
# installed DLL.
set(AEGISUB_CORE_API_NEEDS_FONTCONFIG OFF)
if(UNIX AND NOT APPLE)
    set(AEGISUB_CORE_API_NEEDS_FONTCONFIG ON)
elseif(APPLE AND Fontconfig_FOUND)
    set(AEGISUB_CORE_API_NEEDS_FONTCONFIG ON)
endif()

configure_package_config_file(
    cmake/AegisubCoreCAPIConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AegisubCoreCAPIConfig.cmake"
    INSTALL_DESTINATION lib/cmake/AegisubCoreCAPI
)
install(EXPORT AegisubCoreCAPITargets
    NAMESPACE Aegisub::
    FILE AegisubCoreCAPITargets.cmake
    DESTINATION lib/cmake/AegisubCoreCAPI
    COMPONENT CoreAPI
)
install(EXPORT AegisubCoreCAPISharedTargets
    NAMESPACE Aegisub::
    FILE AegisubCoreCAPISharedTargets.cmake
    DESTINATION lib/cmake/AegisubCoreCAPI
    COMPONENT CoreAPI
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AegisubCoreCAPIConfig.cmake"
    DESTINATION lib/cmake/AegisubCoreCAPI
    COMPONENT CoreAPI
)
# Findass.cmake is no longer required by the Core C API package (libass is
# runtime-loaded). Keep Finduchardet for static consumers that still need it.
install(FILES cmake/Finduchardet.cmake
    DESTINATION lib/cmake/AegisubCoreCAPI/modules
    COMPONENT CoreAPI
)

# Build translations
find_package(Gettext)
option(WITH_TRANSLATION "Build translations" ${GETTEXT_FOUND})
if(WITH_TRANSLATION)
    find_package(Gettext REQUIRED)
    file(READ po/LINGUAS LINGUAS)
    string (STRIP "${LINGUAS}" LINGUAS)
    string (REPLACE " " ";" LINGUAS "${LINGUAS}")
    foreach (_lang ${LINGUAS})
        set(_gmoFile ${CMAKE_CURRENT_BINARY_DIR}/${_lang}.gmo)
        add_custom_command(
            OUTPUT ${_gmoFile}
            COMMAND ${GETTEXT_MSGMERGE_EXECUTABLE} --quiet --update --backup=none --no-fuzzy-matching --sort-by-file ${_lang}.po aegisub.pot
            COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${_gmoFile} ${_lang}.po
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/po
        )
        install(FILES ${_gmoFile} DESTINATION share/locale/${_lang}/LC_MESSAGES RENAME aegisub.mo)
        set(_gmoFiles ${_gmoFiles} ${_gmoFile})
    endforeach ()
    add_custom_target(translations ALL DEPENDS ${_gmoFiles})
endif()

if(WITH_PLUGIN_BRIDGE)
    include(cmake/AegisubPluginBridge.cmake)
endif()

set(WITH_TEST OFF CACHE BOOL "Enable unit tests (gtest / automation)")
set(WITH_SMOKE OFF CACHE BOOL "Enable smoke / process test targets")
if(WITH_TEST OR WITH_SMOKE)
    include(CTest)
endif()
if(WITH_TEST)
    include("CMakeLists.test.txt")
endif()
if(WITH_SMOKE)
    include("CMakeLists.smoke.txt")
endif()

# Optional developer overlay (gitignored). Example:
#   include("${CMAKE_SOURCE_DIR}/benchmarks/CMakeLists.txt")
if(EXISTS "${PROJECT_SOURCE_DIR}/CMakeLists.user.txt")
    include("${PROJECT_SOURCE_DIR}/CMakeLists.user.txt")
endif()

message(STATUS "\n"
    "Configure settings\n"
    "  Install prefix:        ${CMAKE_INSTALL_PREFIX}\n"
    "  CFLAGS                 ${CMAKE_C_FLAGS}\n"
    "  CXXFLAGS               ${CMAKE_CXX_FLAGS}\n"
    "\n"
    "Default Settings\n"
    "  Audio Player:          ${DEFAULT_PLAYER_AUDIO}\n"
    "\n"
    "Audio Players\n"
    "  ALSA:                  ${WITH_ALSA}\n"
    "  CoreAudio:             ${WITH_COREAUDIO}\n"
    "  DirectSound:           ${WITH_DIRECTSOUND}\n"
    "  DirectSound-old:       ${WITH_DIRECTSOUND}\n"
    "  OpenAL:                ${WITH_OPENAL}\n"
    "  OSS:                   ${WITH_OSS}\n"
    "  PortAudio:             ${WITH_PORTAUDIO}\n"
    "  PulseAudio:            ${WITH_LIBPULSE}\n"
    "  XAudio2:               ${WITH_XAUDIO2}\n"
    "\n"
    "Misc Packages\n"
    "  CSRI:                  ${WITH_CSRI}\n"
    "  PFFFT:                 ${WITH_PFFFT}\n"
    "  Hunspell:              ${WITH_HUNSPELL}\n"
    "  uchardet:              ${WITH_UCHARDET}\n"
    "  wxStyledTextCtrl:      ${WITH_WXSTC}\n"
    "  LuaJIT:                bundled\n"
    "  LuaJIT Shared:         ${AEGISUB_LUAJIT_SHARED}\n"
    "  LuaSocket:             ${LUA_WITH_LUASOCKET}\n"
    "  Drawing Skia:          ${WITH_DRAWING_SKIA}\n"
    "  Video tools Skia:      ${WITH_SKIA_VIDEO_TOOLS}\n"
    "  Audio display Skia:    ${WITH_SKIA_AUDIO_DISPLAY}\n"
    "  Subtitle grid Skia:    ${WITH_SKIA_SUBTITLE_GRID}\n"
    "  Software video Skia:   ${WITH_SKIA_SOFTWARE_VIDEO}\n"
    "\n"
    "Dynamic Loading\n"
    "  AviSynth:              ${HAVE_AVISYNTH}\n"
    "  FFMS2:                 ${HAVE_FFMS2}\n"
    "  LsmasNative:           ${HAVE_LSMASNATIVE}\n"
    "  SceneChange:           ${HAVE_SCENECHANGE}\n"
    "  libass:                ${ass_FOUND}\n"
    "  libplacebo:            ${HAVE_LIBPLACEBO}\n"
    "  FFTW3:                 ${WITH_FFTW3}\n"
    "\n"
    "Options\n"
    "  Translations:          ${WITH_TRANSLATION}\n"
    "  Startup log:           ${WITH_STARTUPLOG}\n"
    "  Update checker:        ${WITH_UPDATE_CHECKER}\n"
    "  Plugin Bridge:         ${WITH_PLUGIN_BRIDGE}\n"
    "  Unit tests:            ${WITH_TEST}\n"
    "  Smoke tests:           ${WITH_SMOKE}\n"
    "\n"
)
