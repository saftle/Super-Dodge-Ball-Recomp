cmake_minimum_required(VERSION 3.20)
project(super_dodgeball C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(NESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/../external/nesrecomp)
set(NESRECOMP_BIN ${NESRECOMP_ROOT}/recompiler/build/NESRecomp)
set(CMAKE_CURRENT_SOURCE_DIR ${CMAKE_SOURCE_DIR})
set(GAME_TOML ${CMAKE_CURRENT_SOURCE_DIR}/game.toml)
set(GAME_PREFIX "Super_Dodge_Ball_(USA)")
set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/../generated)
set(ROM_PATH "${CMAKE_SOURCE_DIR}/../roms/Super Dodge Ball (USA).nes")

include(${NESRECOMP_ROOT}/runner/runner.cmake)

# Use bundled SDL2 headers from the NESRecomp runner
include_directories(${CMAKE_SOURCE_DIR}/../src/third_party/SDL2)
include_directories(${NESRECOMP_RUNNER_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/../external/nesrecomp/runner/external/SDL2/include)

# System SDL2 via its CMake config (libsdl2-dev or equivalent) — portable
# across distros, no machine-specific library paths.
find_package(SDL2 REQUIRED)
set(SDL2_LIBRARY SDL2::SDL2)

# Recompile game ROM → C via NESRecomp before building.
# This ensures generated/ files are always fresh when game.toml changes.
add_custom_command(
    OUTPUT ${GENERATED_DIR}/${GAME_PREFIX}_full.c
           ${GENERATED_DIR}/${GAME_PREFIX}_dispatch.c
    COMMAND ${NESRECOMP_BIN} "${ROM_PATH}" --game ${GAME_TOML} --output-prefix ${GAME_PREFIX}
    DEPENDS ${NESRECOMP_BIN} "${ROM_PATH}" ${GAME_TOML}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
    COMMENT "Running NESRecomp to regenerate C code from ROM + game.toml"
    VERBATIM
)

# All generated source files (generated/ is at project root)
# Only use _full.c and _dispatch.c — full.c already contains all bank code
file(GLOB GENERATED_SOURCES
    "${GENERATED_DIR}/${GAME_PREFIX}_full.c"
    "${GENERATED_DIR}/${GAME_PREFIX}_dispatch.c"
)

add_executable(super_dodgeball
    ${NESRECOMP_RUNNER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/extras.c
    ${GENERATED_SOURCES}
)
target_include_directories(super_dodgeball PRIVATE
    ${NESRECOMP_RUNNER_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/nesrecomp/runner/external/SDL2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/third_party/SDL2
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(super_dodgeball PRIVATE ${SDL2_LIBRARY} m)
