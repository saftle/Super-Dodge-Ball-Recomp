# CMake toolchain: Windows x64, cross-compiled from Linux with mingw-w64.
#
# WHY A CROSS BUILD AND NOT CI. The release build needs the ROM, because it
# recompiles the cartridge's code. The ROM does not belong in GitHub -- not
# even as a secret, because one secret cannot hold it: its base64 is 349,548
# bytes and a GitHub Actions secret is capped at 48 KB. So the ROM stays on
# the machine that owns it, and the Windows artifact is built there too.
#
# The cost is honest: this is a mingw binary, not an MSVC one. A different
# CRT, a different SDL2.dll and a different compiler from the artifact
# build.ps1 produces. Both routes should exist.
#
# TWO THINGS THIS FILE HAS TO GET RIGHT.
#
#   * The RECOMPILER must be a HOST program. The engine's project.cmake
#     refuses a cross build without NESRECOMP_HOST_COMPILER, because its own
#     bootstrap would otherwise build a *Windows* recompiler and then try to
#     run it to generate the code -- which cannot run. build-mingw.sh builds
#     the host recompiler for Linux first and passes it in.
#
#   * find_package() must still see the HOST's tools. The code generator is
#     run by the host's python3, so CMAKE_FIND_ROOT_PATH_MODE_PROGRAM is
#     NEVER: a program is a host program even in a cross build. Libraries and
#     headers are the target's alone (ONLY), so a Linux .so can never be
#     linked into a PE.
#
# SDL2 is NOT set here. The engine vendors an MSVC SDL2 and puts its cmake
# dir on CMAKE_PREFIX_PATH, and a mingw build must not pick that up: its
# import library is an MSVC .lib and the link fails, or worse, it links and
# the exe cannot run. build-mingw.sh passes -DSDL2_DIR= at the mingw package
# and then checks that the cache really took.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_TRIPLE "x86_64-w64-mingw32")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_TRIPLE}-g++")
set(CMAKE_RC_COMPILER  "${TOOLCHAIN_TRIPLE}-windres")
set(CMAKE_AR           "${TOOLCHAIN_TRIPLE}-ar"     CACHE FILEPATH "archiver")
set(CMAKE_RANLIB       "${TOOLCHAIN_TRIPLE}-ranlib" CACHE FILEPATH "ranlib")
set(CMAKE_STRIP        "${TOOLCHAIN_TRIPLE}-strip"  CACHE FILEPATH "strip")
set(CMAKE_LINKER       "${TOOLCHAIN_TRIPLE}-ld"     CACHE FILEPATH "linker")

# The cross compiler's sysroot: the target's own headers and import
# libraries live here, and nothing of the host's may leak in.
set(CMAKE_FIND_ROOT_PATH "/usr/${TOOLCHAIN_TRIPLE}")

# The C++ runtime and the unwinder are DLLs (libstdc++-6.dll,
# libgcc_s_seh-1.dll). A published build that needs two DLLs beside the exe
# is a published build that does not start on a clean machine, so both are
# linked in statically. The MSVC route's equivalent is
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded; this is the mingw spelling.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# The same protections MSVC gives a Release build by default.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CMAKE_EXE_LINKER_FLAGS_INIT} -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va")

# A console subsystem, like the MSVC build: the engine's main() is main(),
# not WinMain, and the game prints its startup log and its --frames report
# to the console. A GUI-subsystem build would need a WinMain nobody wrote.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} -mconsole")

# Programs are host programs: python3 runs the code generator, and it is
# the host's python3 that has the engine's scripts on its path.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Libraries and headers are the target's alone.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Packages may be either: the target's (SDL2, by explicit -DSDL2_DIR) or a
# host one the engine asks for by name.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
