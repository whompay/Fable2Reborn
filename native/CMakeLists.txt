# fable2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fable2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FABLE2_SOURCES
    src/main.cpp
    src/pc_features.cpp
    src/pc_perf.cpp
    src/glyph_system.cpp
    src/input_bindings.cpp
    src/cursor_lock.cpp
    src/raw_mouse.cpp
    src/pc_settings.cpp
    src/diagnostics/fable_draw_capture.cpp
    src/diagnostics/fable_native_poc.cpp
    src/diagnostics/fable_native_d3d12_backend.cpp
)

if(WIN32)
    add_executable(fable2 WIN32 ${FABLE2_SOURCES})
else()
    add_executable(fable2 ${FABLE2_SOURCES})
endif()

rexglue_setup_target(fable2 GPU_PLUGINS xenos)

# [ICON] (2026-11-16) App icon (assets/fable2.ico, the "II Reborn" emblem). SDL uses the exe's
# first icon for the window and taskbar. Only if a resource compiler is available: without one
# fable2.exe still builds, just without an icon.
if(WIN32)
    include(CheckLanguage)
    check_language(RC)
    if(CMAKE_RC_COMPILER)
        enable_language(RC)
        set(_fable2_icon_rc "${CMAKE_CURRENT_BINARY_DIR}/fable2_icon.rc")
        file(WRITE "${_fable2_icon_rc}" "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/assets/fable2.ico\"\n")
        target_sources(fable2 PRIVATE "${_fable2_icon_rc}")
    else()
        message(STATUS "fable2: no RC compiler found, building without the app icon")
    endif()
endif()

# This machine supports AVX2/FMA. Keep rounding-sensitive game math strict;
# explicit std::fma calls may become hardware FMA, other operations must not fuse.
option(FABLE2_AVX2 "Optimize recompiled game code for AVX2/FMA-capable PCs" ON)
if(FABLE2_AVX2)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(fable2_recomp PRIVATE -mavx2 -mfma -ffp-contract=off)
    else()
        message(FATAL_ERROR "FABLE2_AVX2 currently requires the Clang/GNU build path")
    endif()
endif()

# Run after codegen and before compiling generated sources. Only reviewed local
# branch misclassifications are repaired; unknown cases fail the build.
find_program(FABLE2_PYTHON NAMES python python3
    HINTS "$ENV{USERPROFILE}/.cache/codex-runtimes/codex-primary-runtime/dependencies/python"
    REQUIRED)
add_custom_target(fable2_local_branch_validation
    COMMAND "${FABLE2_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/repair_local_branches.py"
            --project "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
add_dependencies(fable2_local_branch_validation fable2_codegen)
add_dependencies(fable2_recomp fable2_local_branch_validation)

if(WIN32)
    target_link_options(fable2 PRIVATE "LINKER:/DEBUG" "LINKER:/MAP")
endif()

# PERF-1006 (2026-10-06): profile-guided optimisation (see build_pgo.ps1).
#   FABLE2_PGO=gen -> instrumented exe (game code + project code), writes
#                     pgo_fable2.profraw next to the exe every 30 s and on close
#   FABLE2_PGO=use -> optimised with FABLE2_PGO_PROFILE (llvm-profdata merge output)
# build_pgo.ps1 uses separate build folders, so the normal build is untouched.
set(FABLE2_PGO "off" CACHE STRING "Profile-guided optimisation: off, gen or use")
set(FABLE2_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/pgo/fable2.profdata" CACHE FILEPATH "Merged PGO profile")
if(FABLE2_PGO STREQUAL "gen")
    target_compile_options(fable2_recomp PRIVATE -fprofile-generate)
    target_compile_options(fable2 PRIVATE -fprofile-generate)
    target_compile_definitions(fable2 PRIVATE FABLE2_PGO_GEN=1)
    target_link_options(fable2 PRIVATE -fprofile-generate)
elseif(FABLE2_PGO STREQUAL "use")
    if(NOT EXISTS "${FABLE2_PGO_PROFILE}")
        message(FATAL_ERROR "FABLE2_PGO=use but ${FABLE2_PGO_PROFILE} does not exist")
    endif()
    set(_fable2_pgo_use "-fprofile-use=${FABLE2_PGO_PROFILE}"
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
        -Wno-profile-instr-missing -Wno-backend-plugin)
    target_compile_options(fable2_recomp PRIVATE ${_fable2_pgo_use})
    target_compile_options(fable2 PRIVATE ${_fable2_pgo_use})
    target_compile_definitions(fable2 PRIVATE FABLE2_PGO_USE=1)
endif()
