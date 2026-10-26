cmake_minimum_required(VERSION 3.20)
project(shangon C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Your own copy of the game; nothing from it is stored in this repository.
set(SHO_ROM "${CMAKE_SOURCE_DIR}/sho.md" CACHE FILEPATH "Super Hang-On ROM (GM 00004011-00)")
set(SHO_ROM_SHA1 "ecfd7b3bf4dcbee472ddf2f9cdbe968a05b814e0")

find_package(SDL3 CONFIG)
if(NOT SDL3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
endif()

if(NOT EXISTS "${SHO_ROM}")
    message(FATAL_ERROR "ROM not found: ${SHO_ROM} (pass -DSHO_ROM=path/to/rom)")
endif()
file(SHA1 "${SHO_ROM}" rom_sha1)
if(NOT rom_sha1 STREQUAL SHO_ROM_SHA1)
    message(FATAL_ERROR "${SHO_ROM} has SHA-1 ${rom_sha1}, expected ${SHO_ROM_SHA1}")
endif()

# All of the game is handwritten C, the Z80 sound driver included; the
# recompiled originals are only used by the Makefile's verification builds.

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/game/*.c")

add_executable(shangon
    src/port/main.c
    src/port/options.c
    src/runtime/genesis.c
    src/runtime/vdp.c
    src/runtime/sound_host.c
    src/runtime/verify.c
    src/runtime/entry_native.c
    ${GAME_SOURCES}
    src/sound/ym2612.c
    src/sound/psg.c
    src/sound/driver.c)
target_include_directories(shangon PRIVATE src/runtime src/game src/sound)
target_link_libraries(shangon PRIVATE SDL3::SDL3)
if(NOT MSVC)
    target_link_libraries(shangon PRIVATE m)
endif()
target_compile_definitions(shangon PRIVATE _USE_MATH_DEFINES _DEFAULT_SOURCE)
set_target_properties(shangon PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(shangon PRIVATE /W3 /wd4146)
else()
    target_compile_options(shangon PRIVATE -Wall -Wno-unused-label -Wno-unused-variable)
endif()
if(WIN32)
    set_target_properties(shangon PROPERTIES WIN32_EXECUTABLE ON)
endif()
