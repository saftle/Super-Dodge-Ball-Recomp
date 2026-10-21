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
#         src/sdb_cheats.c       libretro .cht cheats (a separate unit: it
#                                writes guest RAM, the other three do not)
#   - a post-build copy of mods/ beside the executable (preloaded packages)
#   - ... and of cheats/, for the same reason RetroArch keeps a cheats/ dir
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
# The id the mod manifests target. FORCED, not merely defaulted: the
# manifests in mods/packages/ declare this id, and a CMake cache entry left
# over from an earlier configure would keep the OLD id while the manifests
# name the new one -- and then every mod silently refuses to load, with
# "This feature does not support the selected stock ROM" and nothing in the
# build output to explain it.
set(NESRECOMP_GAME_ID "super-dodge-ball" CACHE STRING "Id mod manifests target" FORCE)
# The executable's FILE name. The CMake target stays "nes_game" -- that is
# the engine's convention, and this project's own tools and gates refer to
# build-cycle/nes_game -- but the SHIPPED file can carry the project's name.
# A release build passes -DNESRECOMP_EXE_NAME=SuperDodgeBallRecomp.
set(NESRECOMP_EXE_NAME "nes_game" CACHE STRING "Name of the built executable")
option(NESRECOMP_HEADLESS "Build without SDL window/audio support" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/external/nesrecomp/runner/cyc/project.cmake")

# Release identity. The engine declares NESRECOMP_GAME_VERSION and
# NESRECOMP_BUILD_REVISION as cache variables and bakes them into the binary
# (CYC_BUILD_VERSION / CYC_BUILD_REVISION), where the startup log and the
# launcher's diagnostics read them. Fill them in from the repo's VERSION file
# and from git, unless the caller set them explicitly.
if(NOT NESRECOMP_GAME_VERSION OR NESRECOMP_GAME_VERSION STREQUAL "development")
    set(SDB_VERSION "development")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" SDB_VERSION LIMIT_COUNT 1)
        string(STRIP "${SDB_VERSION}" SDB_VERSION)
    endif()
    if(SDB_VERSION)
        set(NESRECOMP_GAME_VERSION "${SDB_VERSION}" CACHE STRING "Game release version" FORCE)
    endif()
endif()
if(NOT NESRECOMP_BUILD_REVISION OR NESRECOMP_BUILD_REVISION STREQUAL "unknown")
    find_program(SDB_GIT git)
    if(SDB_GIT)
        execute_process(COMMAND "${SDB_GIT}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
            RESULT_VARIABLE SDB_GIT_RESULT OUTPUT_VARIABLE SDB_GIT_OUT
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(SDB_GIT_RESULT EQUAL 0 AND SDB_GIT_OUT)
            set(NESRECOMP_BUILD_REVISION "${SDB_GIT_OUT}" CACHE STRING "Source revision in diagnostic logs" FORCE)
        endif()
    endif()
endif()
message(STATUS "nes_game: version ${NESRECOMP_GAME_VERSION}, revision ${NESRECOMP_BUILD_REVISION}")

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
               "${CMAKE_CURRENT_LIST_DIR}/src/sdb_cheats.c"
    ${options})

# The executable's file name. OUTPUT_NAME, not the target's name: renaming
# the target would move build-cycle/nes_game and break every tool and gate
# that refers to it.
if(NOT NESRECOMP_EXE_NAME STREQUAL "nes_game")
    set_target_properties(nes_game PROPERTIES OUTPUT_NAME "${NESRECOMP_EXE_NAME}")
endif()

# The mod catalog lives beside the executable. Preload the shipped packages so
# the feature is present (and default-on) on a fresh build.
#
# The destination is cleared first: copy_directory merges, so a package that
# was RENAMED or removed would leave its old directory behind in the build
# tree, and the launcher's Mods screen would list both the old and the new
# one. A stale entry there is not cosmetic -- the Mods screen offers a
# package the build no longer has.
add_custom_command(TARGET nes_game POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "$<TARGET_FILE_DIR:nes_game>/mods"
        "$<TARGET_FILE_DIR:nes_game>/cheats"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/mods" "$<TARGET_FILE_DIR:nes_game>/mods"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/cheats" "$<TARGET_FILE_DIR:nes_game>/cheats"
    COMMENT "Preloading mod packages and cheats beside nes_game")

# The launcher copies the hero art into the staged assets in a POST_BUILD
# step, so a changed box art would otherwise never reach the build: the
# target stays up to date and the copy never runs. Making the target depend
# on the file relinks it, and the copy runs with it.
if(NESRECOMP_BOXART)
    set_property(TARGET nes_game APPEND PROPERTY LINK_DEPENDS "${NESRECOMP_BOXART}")
endif()