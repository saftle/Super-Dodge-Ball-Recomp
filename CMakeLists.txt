# Super Dodge Ball — cycle backend game project.
#
# Mirrors external/nesrecomp/runner/cyc/project/CMakeLists.txt, which cannot be
# used directly: it declares only C (the mod runtime is C++) and has no way to
# pass MODS / GAME_ID / HOST_EXTRAS. The submodule must stay pristine, so the
# game project lives here.
#
# Differences from the shared project/CMakeLists.txt:
#   - project(... C CXX)                      the mod runtime is C++
#   - nesrecomp_add_cycle_game(... MODS GAME_ID <id>)   launcher's Mods screen
#   - ... HOST_EXTRAS src/sdb_host_extras.c    the no-sprite-limit mod
#   - a post-build copy of mods/ beside the executable (preloaded packages)
cmake_minimum_required(VERSION 3.20)
project(NESRecompCycleGame C CXX)

set(NESRECOMP_ROM "" CACHE FILEPATH "Existing game ROM (required)")
set(NESRECOMP_GAME_CONFIG "" CACHE FILEPATH "Existing game.toml (optional)")
set(NESRECOMP_CYCLE_SEEDS "" CACHE FILEPATH "Cycle seed file override (optional)")
set(NESRECOMP_CYCLE_CAPTURES "" CACHE FILEPATH "Cycle RAM capture file override (optional)")
set(NESRECOMP_HOST_COMPILER "" CACHE FILEPATH "Prebuilt host NESRecomp (optional)")
set(NESRECOMP_FDS_BIOS "" CACHE FILEPATH "FDS BIOS (optional; default game.toml [fds] bios)")
set(NESRECOMP_BOXART "" CACHE FILEPATH "Launcher box art, 24-bit .tga (optional)")
# Manifests target this id plus the image identity (CRC-32 of the cartridge
# payload after the iNES header): "689971f9" for roms/Super Dodge Ball (USA).nes.
set(NESRECOMP_GAME_ID "super-dodgeball" CACHE STRING "Id mod manifests target")
option(NESRECOMP_HEADLESS "Build without SDL window/audio support" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/external/nesrecomp/runner/cyc/project.cmake")

set(options "")
if(NESRECOMP_HEADLESS)
    list(APPEND options HEADLESS)
endif()
if(NESRECOMP_BOXART)
    list(APPEND options BOXART "${NESRECOMP_BOXART}")
endif()

nesrecomp_add_cycle_game(nes_game
    ROM "${NESRECOMP_ROM}"
    GAME_CONFIG "${NESRECOMP_GAME_CONFIG}"
    SEED_FILE "${NESRECOMP_CYCLE_SEEDS}"
    CAPTURE_FILE "${NESRECOMP_CYCLE_CAPTURES}"
    RECOMPILER "${NESRECOMP_HOST_COMPILER}"
    BIOS "${NESRECOMP_FDS_BIOS}"
    MODS
    GAME_ID "${NESRECOMP_GAME_ID}"
    HOST_EXTRAS "${CMAKE_CURRENT_LIST_DIR}/src/sdb_host_extras.c"
    ${options})

# The mod catalog lives beside the executable. Preload the shipped packages so
# the feature is present (and default-on) on a fresh build.
add_custom_command(TARGET nes_game POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/mods" "$<TARGET_FILE_DIR:nes_game>/mods"
    COMMENT "Preloading mod packages beside nes_game")