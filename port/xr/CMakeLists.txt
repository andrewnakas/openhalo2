cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Halo 2 - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

# Static C runtime: a player needs no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(halo2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "D:/n64work/halo2/third_party/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
# OPENHALO2_GEN_DIR: build from another gen dir (a private copy post-passed by
# h2lift/fe_pass.py for the decomp-assist bench); default the normal one.
set(OPENHALO2_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH "Generated (lifted) C directory")
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${OPENHALO2_GEN_DIR}/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/mmio_direct.c
    src/nvnet.c
    src/byog.c
    src/openlive.c
    src/openlive_api.c
    src/openlive_match.c
    src/openlive_friends.c
    src/openlive_clans.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/import/h2import.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
# The toolkit's libraries, named one by one rather than through the
# 'xboxrecomp' umbrella: the umbrella also carries xbox_apu (code extracted
# from xemu, LGPL) and xbox_dsound (which links it). The APU is the
# clean-room model in src/mcpxaudio; the previous one is only linked for A/B
# with -DOPENHALO2_WITH_OLD_APU=ON (see below).
target_link_libraries(${PROJECT_NAME} PRIVATE
    xbox_kernel xbox_d3d8 xbox_input xbox_usb xbox_nv2a xbox_video)
# xboxrecomp.h includes the HLE DirectSound header; only the header is used.
target_include_directories(${PROJECT_NAME} PRIVATE ${XBOXRECOMP_DIR}/src/audio)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/import
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
    ws2_32
    comdlg32
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # Function addresses for tools/sample_prof.py (a sampling profiler).
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        # 32-bit host: the guest's RAM is mapped low (the first free of
        # 0x10000, 8 MB, 16 MB, 32 MB, 256 MB) and its contiguous window at
        # 0x80000000 + that offset, so the program itself sits out of the way.
        target_link_options(${PROJECT_NAME} PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO)
    endif()
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# Decomp-assist bench (docs/DECOMP_ASSIST_PLAN.md): function-entry counters,
# argument samples and state snapshots. Needs OPENHALO2_GEN_DIR pointing at a
# gen copy passed by h2lift/fe_pass.py. OFF = release, unchanged.
option(OPENHALO2_FECHECK "Instrumented build for the decomp-assist bench" OFF)
if(OPENHALO2_FECHECK)
    target_sources(${PROJECT_NAME} PRIVATE src/fecheck_rt.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_FECHECK=1)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE ${OPENHALO2_GEN_DIR})
endif()

# Online system link (net/h2net.c, libdatachannel from vcpkg, static).
# Built in when the vcpkg tree is there; RECOMP_NET_ROOM picks a room at run time.
set(H2NET_VCPKG "D:/n64work/vcpkg/installed/x64-windows-static" CACHE PATH "vcpkg tree with libdatachannel")
option(OPENHALO2_NET "Online system link transport" ON)
if(WIN32 AND OPENHALO2_NET AND EXISTS "${H2NET_VCPKG}/lib/datachannel.lib")
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../net/h2net.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../net/olive.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE OPENHALO2_NET=1 RTC_STATIC=1)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../net ${H2NET_VCPKG}/include)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${H2NET_VCPKG}/lib/datachannel.lib ${H2NET_VCPKG}/lib/juice.lib
        ${H2NET_VCPKG}/lib/usrsctp.lib ${H2NET_VCPKG}/lib/libssl.lib
        ${H2NET_VCPKG}/lib/libcrypto.lib iphlpapi crypt32 bcrypt user32 advapi32)
    message(STATUS "Online system link: libdatachannel from ${H2NET_VCPKG}")
endif()

# Clean-room APU model (src/mcpxaudio, docs/APU_CLEANROOM_DESIGN.md). With
# OPENHALO2_WITH_OLD_APU=ON the previous model (xbox_apu) is linked as well and
# RECOMP_APU_IMPL=old|new|shadow picks at run time (src/apu_select.c); off,
# xbox_apu and xbox_dsound are not even built.
option(OPENHALO2_WITH_OLD_APU "Also link the previous (xemu-derived) APU model for A/B" OFF)
add_library(mcpx_audio STATIC
    src/mcpxaudio/ma_regs.c
    src/mcpxaudio/ma_voice.c
    src/mcpxaudio/ma_mix.c
    src/mcpxaudio/ma_fx.c
    src/mcpxaudio/ma_thread.c
    src/mcpxaudio/ma_softmix.c
    src/mcpxaudio/ma_contract.c
    src/mcpxaudio/apu_xaudio2.c
)
target_include_directories(mcpx_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/mcpxaudio
    PRIVATE ${XBOXRECOMP_DIR}/src)
target_compile_definitions(mcpx_audio PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(mcpx_audio PRIVATE /W3 /O2 /fp:fast)
endif()
target_link_libraries(mcpx_audio PRIVATE winmm xaudio2_8 ole32)
target_sources(${PROJECT_NAME} PRIVATE src/apu_select.c)
target_link_libraries(${PROJECT_NAME} PRIVATE mcpx_audio)
if(OPENHALO2_WITH_OLD_APU)
    target_link_libraries(${PROJECT_NAME} PRIVATE xbox_apu)
    # only this file: a target-wide definition would rebuild the generated code
    set_source_files_properties(src/apu_select.c PROPERTIES COMPILE_DEFINITIONS OPENHALO2_OLD_APU=1)
else()
    # the contract symbols (g_apu_state, mcpx_apu_*, apu_mixer_*) come from here
    target_compile_definitions(mcpx_audio PRIVATE MA_CONTRACT=1)
    set_target_properties(xbox_apu xbox_dsound PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
