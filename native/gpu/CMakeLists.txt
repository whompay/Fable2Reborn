cmake_minimum_required(VERSION 3.25)
project(Fable2Gpu LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(rexglue CONFIG REQUIRED)
set(REXGLUE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../tools/rexglue-source")
set(REXGLUE_USE_D3D12 ON)
set(REXGLUE_USE_VULKAN OFF)
add_library(rexruntime ALIAS rex::runtime)
add_library(xxHash::xxhash ALIAS rex::xxhash)
# Reuse the SDK's exact source list, linking its installed runtime.
function(rex_target_pch target)
endfunction()
add_subdirectory("${REXGLUE_ROOT}/src/graphics" graphics)
target_compile_options(rexgpu-xenos PRIVATE -msse4.1)

# PERF-1006 (2026-10-06): Fable-only helpers (high-resolution GPU wait, build
# marker, PGO profile writer). Kept out of the SDK sources on purpose.
target_sources(rexgpu-xenos PRIVATE "${CMAKE_CURRENT_LIST_DIR}/fable2_gpu_perf.cpp")

# PERF-1006: the GPU command thread is the frame-rate ceiling, but this DLL was
# built for plain SSE4.1 while fable2.exe already requires AVX2/FMA. Same CPU
# floor as the exe; no fast-math, no contraction of separate multiply/adds.
option(FABLE2_GPU_AVX2 "Build the GPU plugin for AVX2/FMA CPUs (same floor as fable2.exe)" ON)
if(FABLE2_GPU_AVX2)
    target_compile_options(rexgpu-xenos PRIVATE -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -ffp-contract=off)
endif()

# PERF-1006: profile-guided optimisation of the GPU plugin (see native/build_pgo.ps1).
#   FABLE2_PGO=gen -> instrumented DLL, writes pgo_rexgpu.profraw next to itself every 30 s
#   FABLE2_PGO=use -> optimised with FABLE2_GPU_PGO_PROFILE (llvm-profdata merge output)
set(FABLE2_PGO "off" CACHE STRING "Profile-guided optimisation: off, gen or use")
set(FABLE2_GPU_PGO_PROFILE "${CMAKE_CURRENT_LIST_DIR}/../pgo/rexgpu.profdata" CACHE FILEPATH "Merged GPU PGO profile")
if(FABLE2_PGO STREQUAL "gen")
    target_compile_options(rexgpu-xenos PRIVATE -fprofile-generate -DFABLE2_GPU_PGO_GEN=1)
    target_link_options(rexgpu-xenos PRIVATE -fprofile-generate)
elseif(FABLE2_PGO STREQUAL "use")
    if(NOT EXISTS "${FABLE2_GPU_PGO_PROFILE}")
        message(FATAL_ERROR "FABLE2_PGO=use but ${FABLE2_GPU_PGO_PROFILE} does not exist")
    endif()
    target_compile_options(rexgpu-xenos PRIVATE "-fprofile-use=${FABLE2_GPU_PGO_PROFILE}" -DFABLE2_GPU_PGO_USE=1
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
endif()
add_library(morph_filter_test OBJECT morph_filter_test.cpp)
