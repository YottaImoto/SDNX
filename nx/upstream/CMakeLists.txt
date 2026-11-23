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
    "${sdnx_common}/sdnx_malloc.cpp")
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
rexglue_switch_add_nro(sdretail NAME "Sleeping Dogs" AUTHOR "SDNX" VERSION "16.230"
                       ICON "${CMAKE_CURRENT_LIST_DIR}/../sdretail/icon.jpg")
