# Built as a subdirectory of the static Switch SDK.
set(SDNX_GENERATED_DIR "" CACHE PATH "Directory containing the generated Sleeping Dogs sources")
include("${SDNX_GENERATED_DIR}/sources.cmake")
add_library(sdretail_guest OBJECT ${GENERATED_SOURCES})
target_link_libraries(sdretail_guest PRIVATE rexppc rexkernel)
target_include_directories(sdretail_guest PRIVATE "${SDNX_GENERATED_DIR}")
target_compile_definitions(sdretail_guest PRIVATE REX_GUEST_VOLATILE=volatile)
# SDNX 16.230: -O3 like the fork's builds (the game's threads take all three cores); the registration table and the
# init file stay at -O1 (82k registrations, no hot path, tens of minutes at -O3).
target_compile_options(sdretail_guest PRIVATE -O3 -fno-strict-aliasing -ffp-contract=off)
set_source_files_properties("${SDNX_GENERATED_DIR}/sdretail_register.cpp" "${SDNX_GENERATED_DIR}/sdretail_init.cpp"
    PROPERTIES COMPILE_OPTIONS "-O1")
target_precompile_headers(sdretail_guest PRIVATE "${SDNX_GENERATED_DIR}/sdretail_pch.h")

add_executable(sdretail src/main.cpp $<TARGET_OBJECTS:sdretail_guest>)
set(sdnx_common "${CMAKE_CURRENT_LIST_DIR}/../sdretail/src")
target_sources(sdretail PRIVATE "${sdnx_common}/sdnx_perf_hooks.cpp" "${sdnx_common}/sdnx_fps_limit.cpp"
    "${sdnx_common}/sdnx_alloc_trace.cpp" "${sdnx_common}/sdnx_verify_data.cpp" "${sdnx_common}/sdnx_memset.cpp"
    "${sdnx_common}/sdnx_malloc.cpp" "${sdnx_common}/sdnx_pgo.cpp" "${sdnx_common}/sdnx_world_density.cpp"
    "${sdnx_common}/sdnx_fog_interval.cpp" "${sdnx_common}/sdnx_vehicle_skin_cache.cpp"
    "${sdnx_common}/sdnx_cull_profile.cpp")
target_link_libraries(sdretail PRIVATE xxHash::xxhash)  # sdnx_vehicle_skin_cache.cpp
target_compile_definitions(sdretail PRIVATE SDNX_UPSTREAM_SDK=1)
target_link_options(sdretail PRIVATE LINKER:--wrap=memset
    LINKER:--wrap=_malloc_r,--wrap=_free_r,--wrap=_realloc_r,--wrap=_calloc_r,--wrap=_memalign_r,--wrap=_malloc_usable_size_r)
target_include_directories(sdretail PRIVATE "${SDNX_GENERATED_DIR}")
set(sdnx_modules rexbase rexppc rexxenos rexfilesystem rexkernel rexapu rexhid rexui rexgpu
    rexgpu-xenos-base rexgpu-xenos-sdnx rexxam rexxbdm rexapp rexapu-switch rexhid-switch rexui-switch)
foreach(module IN LISTS sdnx_modules)
  list(APPEND sdnx_archives "$<TARGET_FILE:${module}>")
endforeach()
string(JOIN "," sdnx_archives_csv ${sdnx_archives})
target_link_options(sdretail PRIVATE "LINKER:--whole-archive,${sdnx_archives_csv},--no-whole-archive")
target_link_libraries(sdretail PRIVATE ${sdnx_modules} rexglue_switch_startup)
# SDNX 16.235: the modules whose sources compile in an OBJECT library of their own (<module>-objects): the kernel,
# XAM, file system, app, audio, input and UI. Compile options for whole-program builds (LTO, PGO) go to these too;
# 16.232-16.234 left them out of LTO (renut base port 89 found the same).
set(sdnx_compile_targets ${sdnx_modules})
foreach(module IN LISTS sdnx_modules)
  if(TARGET ${module}-objects)
    list(APPEND sdnx_compile_targets ${module}-objects)
  endif()
endforeach()
# SDNX 16.235: profile-guided optimisation (the fork's 16.188-16.190; renut base: PGO + LTO were SASRT's biggest CPU
# win). generate: the game code, the app and the SDK modules count their branches and calls; sdnx_pgo.cpp writes the
# counts every 2 minutes to <NRO folder>/pgo/sdnx_pgo_<N>.stream (with -fprofile-info-section, no .gcda at exit);
# nx/tools/sd_pgo_merge.sh turns a stream into .gcda files in SDNX_PGO_DIR. use: optimised with those counts (with
# SDNX_LTO in the same tree). -fprofile-prefix-path names the .gcda by the path inside the build tree, so the
# generate and use trees may differ. Each mode in a tree of its own (REXGLUE_OUTPUT_SUFFIX).
set(SDNX_PGO "" CACHE STRING "Profile-guided optimisation: empty, generate or use")
set(SDNX_PGO_DIR "$ENV{HOME}/sdnx-next/pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
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
    set(_sdnx_pgo "-fprofile-use=${SDNX_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -fprofile-correction
                  -fno-profile-values -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch)
    target_compile_options(sdretail_guest PRIVATE --param=max-inline-insns-auto=64 --param=inline-unit-growth=12)
  else()
    message(FATAL_ERROR "SDNX_PGO must be empty, generate or use (got ${SDNX_PGO})")
  endif()
  foreach(_target IN ITEMS sdretail sdretail_guest ${sdnx_compile_targets})
    target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${_sdnx_pgo}>")
  endforeach()
  target_link_options(sdretail PRIVATE ${_sdnx_pgo})
  message(STATUS "SDNX: PGO ${SDNX_PGO} (${SDNX_PGO_DIR})")
endif()

# SDNX 16.232: link-time optimisation (the fork's 16.196; renut base: PGO + LTO were SASRT's biggest CPU win). The game
# code, the app and the SDK modules are compiled to GIMPLE and optimised as one program at the link. Build it in a tree
# of its own with -DREXGLUE_OUTPUT_SUFFIX=-lto: GIMPLE-only libraries must not replace the everyday tree's.
# Out of LTO: files with top-level asm naming C symbols and the files defining --wrap's __wrap_ functions (the startup
# object library is not in the list at all), and deko3d_backend.cpp as in the fork.
option(SDNX_LTO "Link-time optimisation of the game code, the app and the SDK modules" OFF)
set(SDNX_LTO_JOBS "6" CACHE STRING "Parallel LTRANS jobs of the LTO link")
if(SDNX_LTO)
  set(_sdnx_lto_targets sdretail sdretail_guest ${sdnx_compile_targets})
  foreach(_target IN LISTS _sdnx_lto_targets)
    target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-flto;-fno-fat-lto-objects>")
    target_compile_definitions(${_target} PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
  endforeach()
  target_link_options(sdretail PRIVATE -flto=${SDNX_LTO_JOBS} -flto-partition=balanced -ffp-contract=off
                      -fno-strict-aliasing -O3)
  set_property(SOURCE "${PROJECT_SOURCE_DIR}/src/base/threading_posix.cpp"
                      "${PROJECT_SOURCE_DIR}/src/base/exception_handler_switch.cpp"
                      "${PROJECT_SOURCE_DIR}/src/base/fiber_switch.cpp"
               TARGET_DIRECTORY rexbase APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
  set_property(SOURCE "${PROJECT_SOURCE_DIR}/src/gpu/xenos/sdnx/deko3d_backend.cpp"
               TARGET_DIRECTORY rexgpu-xenos-sdnx APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
  set_property(SOURCE "${sdnx_common}/sdnx_memset.cpp" "${sdnx_common}/sdnx_malloc.cpp"
               APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
  message(STATUS "SDNX: LTO (${SDNX_LTO_JOBS} LTRANS jobs)")
endif()

rexglue_switch_add_nro(sdretail NAME "Sleeping Dogs" AUTHOR "SDNX" VERSION "16.258"
                       ICON "${CMAKE_CURRENT_LIST_DIR}/../sdretail/icon.jpg")
