cmake_minimum_required(VERSION 3.20)
project(shangon C)

# Your own copy of the game; nothing from it is stored in this repository.
set(SHO_ROM "${CMAKE_SOURCE_DIR}/sho.md" CACHE FILEPATH "Super Hang-On ROM (GM 00004011-00)")
set(SHO_ROM_SHA1 "ecfd7b3bf4dcbee472ddf2f9cdbe968a05b814e0")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
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

# Code generated from the ROM (temporary: replaced function by function with
# handwritten C, see README)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
add_custom_command(
    OUTPUT "${GEN_DIR}/z80_code.c" "${GEN_DIR}/z80_funcs.h"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/z80recomp.py" "${SHO_ROM}" "${GEN_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/z80recomp.py" "${CMAKE_SOURCE_DIR}/tools/z80dis.py"
            "${CMAKE_SOURCE_DIR}/tools/z80.py" "${SHO_ROM}"
    COMMENT "Generating sound driver code from ROM")
add_custom_command(
    OUTPUT "${GEN_DIR}/sho_code.c" "${GEN_DIR}/sho_funcs.h"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/recomp.py" "${SHO_ROM}" "${GEN_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/recomp.py" "${CMAKE_SOURCE_DIR}/tools/shodis.py"
            "${CMAKE_SOURCE_DIR}/tools/m68k.py" "${CMAKE_SOURCE_DIR}/config/hints.txt" "${SHO_ROM}"
    COMMENT "Generating code from ROM")

add_executable(shangon
    src/port/main.c
    src/runtime/genesis.c
    src/runtime/vdp.c
    src/runtime/sound_host.c
    src/sound/ym2612.c
    src/sound/psg.c
    "${GEN_DIR}/sho_code.c"
    "${GEN_DIR}/z80_code.c")
target_include_directories(shangon PRIVATE src/runtime "${GEN_DIR}")
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
