# Sleeping Dogs (Xbox 360 retail) for Nintendo Switch: static recompilation host application.
#
# The game code is not in this repository: generated/ holds the C++ recompiled from your own default.xex
# (rexglue codegen sdretail_manifest.toml). A cross build cannot run rexglue itself.
#
# Build (devkitA64 container):
#   cmake -S sdretail -B out/nx-sd -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXGLUE_SWITCH_NVK_SDK=<mesa sdk>/opt/devkitpro/portlibs/switch
#   cmake --build out/nx-sd -j8
# Output: out/nx-sd/sdretail.nro

cmake_minimum_required(VERSION 3.25)
project(sdretail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

include(generated/rexglue.cmake)

set(SDRETAIL_SOURCES
    src/main.cpp
    src/sdnx_fps_limit.cpp
    src/sdnx_alloc_trace.cpp
    src/sdnx_verify_data.cpp
    src/sdnx_perf_hooks.cpp
    src/sdnx_memset.cpp
    src/sdnx_pgo.cpp
)

add_executable(sdretail ${SDRETAIL_SOURCES})
set_source_files_properties(${SDRETAIL_SOURCES} PROPERTIES COMPILE_OPTIONS "-g1")

# Xenos GPU plugin stays available; no native renderer yet.
rexglue_setup_target(sdretail GPU_PLUGINS xenos)

# The SDK sets these with add_compile_options in its own directory only, so the game code built here did not get
# them. Without -ffp-contract=off GCC fuses separate PPC fmul + fadd into one FMA on aarch64 and the results drift
# by one ulp: the Scaleform hash capacity 1 << int(log2(n - 1) + 1) came out 64 instead of 32 on the console, the
# UI heap went another way and, a few hundred requests later, asked for 0x41FFF928 bytes (first console runs,
# 2026-11-11). The PC build (clang, x86 without FMA) never fuses. The game code itself is in sdretail_recomp.
foreach(_target sdretail sdretail_recomp)
    if(TARGET ${_target})
        target_compile_options(${_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>
            $<$<CXX_COMPILER_ID:GNU>:-fno-strict-aliasing>)
    endif()
endforeach()

# Zero fills of 160+ bytes without newlib's DC ZVA path (src/sdnx_memset.cpp: 10.6 % of the GPU thread on the console).
if(REXGLUE_PLATFORM_SWITCH)
    target_link_options(sdretail PRIVATE -Wl,--wrap=memset)
endif()

# A/B (nx/tools/sd_build_nro_volatile.sh): guest loads/stores volatile again, as on the desktop bench (the default Switch
# build strips them with pch_no_volatile.py; the generated pch keeps REX_GUEST_VOLATILE as the switch).
option(SDRETAIL_GUEST_VOLATILE "Guest memory accesses of the generated code as volatile" OFF)
if(SDRETAIL_GUEST_VOLATILE AND TARGET sdretail_recomp)
    target_compile_definitions(sdretail_recomp PRIVATE REX_GUEST_VOLATILE=volatile)
endif()

# sdretail_register.cpp is a 4 MB table of 82k function registrations: -O3 on it costs tens of
# minutes of compile time and changes nothing at runtime (no hot code path).
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sdretail_register.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sdretail_init.cpp"
    PROPERTIES COMPILE_OPTIONS "-O1")

if(REXGLUE_PLATFORM_SWITCH)
    set(SDRETAIL_VERSION "0.1.0" CACHE STRING "NRO display version")
    # icon.jpg: a 256x256 JPEG made from icon_horizon.png (repository root). It is the game's cover art, so it is not
    # in the repository (.gitignore): without it the NRO gets libnx's default icon.
    set(_sdretail_icon)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
        set(_sdretail_icon ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
    endif()
    rexglue_switch_add_nro(sdretail
        NAME "Sleeping Dogs"
        AUTHOR "SDNX"
        VERSION "${SDRETAIL_VERSION}"
        ${_sdretail_icon})
endif()

# SDNX 16.188: profile-guided optimisation (renut_nx_ultimate_base's method, docs/building.md section 4; on SASRT
# PGO + LTO took the render thread 30.9 -> 24.6 ms and the game thread 29.8 -> 24.6 ms, +9 % fps). Driving here is
# CPU-bound (285 % of the 300 % the process has, 16.184), ~200 % of it in the game's own code.
#   SDNX_PGO=generate: an instrumented NRO (game code + app) that streams its branch counts to <NRO folder>/pgo/
#     (src/sdnx_pgo.cpp; registered in a section, not by constructors). Counters without atomics (lost increments
#     are smoothed by -fprofile-correction at the use step), no value profiles (devkitA64's libgcov is not safe
#     with them: blur-nx).
#   SDNX_PGO=use: the counts merged by nx/tools/sd_pgo_merge.sh from SDNX_PGO_DIR. Code the session did not run is
#     compiled as without a profile (-fprofile-partial-training); functions changed since keep none
#     (-Wno-coverage-mismatch). The game code's inliner limits from sa2-nx (max-inline-insns-auto 64,
#     inline-unit-growth 12).
# -fprofile-prefix-path makes the .gcda names relative to the build folder, so the two builds may use different ones.
# The SDK libraries stay out (they share one output folder between build trees).
set(SDNX_PGO "" CACHE STRING "Profile-guided optimisation: empty, generate or use")
set(SDNX_PGO_DIR "$ENV{HOME}/masseffect-nx/pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
if(SDNX_PGO)
    if(SDNX_PGO STREQUAL "generate")
        set(_sdnx_pgo "-fprofile-generate=${SDNX_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                      -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(sdretail PRIVATE SDNX_PGO_GENERATE=1)
    elseif(SDNX_PGO STREQUAL "use")
        file(GLOB_RECURSE _sdnx_pgo_gcda "${SDNX_PGO_DIR}/*.gcda")
        if(NOT _sdnx_pgo_gcda)
            message(FATAL_ERROR "SDNX_PGO=use: no .gcda in ${SDNX_PGO_DIR} (nx/tools/sd_pgo_merge.sh first)")
        endif()
        set(_sdnx_pgo "-fprofile-use=${SDNX_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                      -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile
                      -Wno-coverage-mismatch)
        if(TARGET sdretail_recomp)
            target_compile_options(sdretail_recomp PRIVATE --param=max-inline-insns-auto=64
                                   --param=inline-unit-growth=12)
        endif()
    else()
        message(FATAL_ERROR "SDNX_PGO must be empty, generate or use (got ${SDNX_PGO})")
    endif()
    foreach(_target sdretail sdretail_recomp)
        if(TARGET ${_target})
            target_compile_options(${_target} PRIVATE ${_sdnx_pgo})
        endif()
    endforeach()
    target_link_options(sdretail PRIVATE ${_sdnx_pgo})
    message(STATUS "SDNX: PGO ${SDNX_PGO} (${SDNX_PGO_DIR})")
endif()
