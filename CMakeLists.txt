cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mega65-banked-nokernal.cmake)
endif()

project(simon65 CXX ASM)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Before any target, or clang-tidy sees an empty database.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SIMON_CARD_DIR "SIMON65"
    CACHE STRING "the directory on the SD card the game reads its data from")
add_compile_definitions(SIMON_CARD_DIR="${SIMON_CARD_DIR}")

# --- the program --------------------------------------------------------------

add_executable(simon65mod
  src/main.cpp
  src/talk.cpp
  src/saves.cpp
  src/speech.cpp
  src/room.cpp
  src/frame.cpp
  src/modplay.S
  src/transpose.S
  src/laycells.S)
set_target_properties(simon65mod PROPERTIES SUFFIX ".prg")
target_include_directories(simon65mod PRIVATE src)
# The banks come off the SD card through Hyppo, the same route as the game's
# own files -- the platform's default, so nothing to define.

# Hyppo stages a filename in a page the linker names, and the default sits at
# $0100 -- the bottom of the hardware stack. $0200 is below ram_low and this
# program puts nothing there.
target_link_options(simon65mod PRIVATE
  -Wl,--defsym=__mega65_h_name_buf=0x0200)

# The near regions are reserved by the linker, not sited in C.
target_link_options(simon65mod PRIVATE
  -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/layout.ld)
set_target_properties(simon65mod PROPERTIES
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/layout.ld)
target_compile_options(simon65mod PRIVATE -Oz -Wall -Wextra)

# --- the banks ----------------------------------------------------------------
#
# An 8 KB window rather than the default 24, and the banks based right after
# Hyppo's workspace. Two reasons, both about what is left over.
#
# The window sits at $2000 and the fixed region starts above it, so shrinking
# the window from 24 KB to 8 grows ram_fixed from 20,480 bytes to 36,864 --
# which is what makes room for the item database near enough to point at.
#
# And the platform's default bases put banks 8-10 at $40000-$4C000, on top of
# the glyphs. Basing them at $12000 on an 8 KB stride puts every bank below
# $1E000 whatever the count, which leaves $20000-$5FFFF -- the only contiguous
# 256 KB the machine has -- for the run of glyphs that must be one piece.
#
# Banks past the chip six live in the Attic and cost no chip RAM at all. The
# SDK checks the address and not the number (_mapper.h:669), so bank 7 at
# $8000000 is as legal as bank 13; they are packed on the window's own 8 KB
# stride rather than the mapper's 24 KB, and atticmap asserts the data starts
# above every slot the mapper can reach. Code there runs about six times slower
# than from chip (banks.hpp), so only cold code goes there.
set(MAPPER_WINDOW_KB 8)
set(CHIP_BANKS 6)
set(ATTIC_BANKS 6)
math(EXPR MAPPER_BANK_COUNT "${CHIP_BANKS} + ${ATTIC_BANKS}")
target_compile_definitions(simon65mod PRIVATE
  MAPPER_WINDOW_KB=${MAPPER_WINDOW_KB} MAPPER_BANK_COUNT=${MAPPER_BANK_COUNT})
set(BANK_DEFINES -DMAPPER_WINDOW_KB=${MAPPER_WINDOW_KB}
                 -DMAPPER_BANK_COUNT=${MAPPER_BANK_COUNT})
foreach(bank RANGE 1 ${MAPPER_BANK_COUNT})
  if(bank GREATER ${CHIP_BANKS})
    math(EXPR base "0x8000000 + (${bank} - ${CHIP_BANKS} - 1) * 0x2000"
         OUTPUT_FORMAT HEXADECIMAL)
  else()
    math(EXPR base "0x12000 + (${bank} - 1) * 0x2000" OUTPUT_FORMAT HEXADECIMAL)
  endif()
  target_compile_definitions(simon65mod PRIVATE MAPPER_BANK_${bank}=${base})
  list(APPEND BANK_DEFINES -DMAPPER_BANK_${bank}=${base})
endforeach()

# --- knobs --------------------------------------------------------------------

# Link the driver's body into an Attic bank instead of the fixed region. Attic
# RAM is about ten times slower; this is how to find out whether a tick still
# fits the frame, and so whether music can cost no chip RAM at all.
#
# Banks 1-6 are based by hand above, so the platform defaults that put 8-10 on
# top of the glyphs never apply. A bank past 6 would, and chipmap.hpp's region
# table is what says so.
set(PEP_ATTIC_BANK OFF CACHE BOOL "put the driver body in an Attic bank")
set(PEP_BANK 13 CACHE STRING "which bank that is; 13 is the first Attic one")
if(PEP_ATTIC_BANK)
  target_compile_definitions(simon65mod PRIVATE
    PEP_ATTIC_BANK PEP_BANK=${PEP_BANK} MAPPER_BANK_COUNT=${PEP_BANK})
endif()

# The player hands the MATH unit back as it found it around every tick, so a tick
# can borrow the multiplier without disturbing anyone else. Only matters once
# something outside the player uses the unit; nothing here does yet.
#
# Measured at 40 MHz: 1.31 rasters a tick against 1.20 without, 0.02% of a
# frame. Left on.
set(PEP_SAVE_MATH ON CACHE BOOL "hand the MATH unit back after each tick")
if(NOT PEP_SAVE_MATH)
  target_compile_definitions(simon65mod PRIVATE PEP_SAVE_MATH=0)
endif()

# Paint the border across a tick, so its cost is visible on a real screen.
set(PEP_SHOW_RASTER OFF CACHE BOOL "border bar across each tick")
if(PEP_SHOW_RASTER)
  target_compile_definitions(simon65mod PRIVATE PEP_SHOW_RASTER=1)
endif()

# --- the program the machine runs ---------------------------------------------
#
# A second, tiny program at BASIC 65's start address: `RUN` reaches it, it
# darkens the screen and silences the audio channels, and then it loads the
# player over itself. Neither of those can be done by the player, because by
# the time the player runs it has already been loaded over the screen.
#
# Built with the SDK's plain platform rather than as a target of this project:
# it wants BASIC, the KERNAL and Hyppo, none of the banks, and CMake takes one
# toolchain to a build.
set(PLAIN_CFG ${MEGA65_SDK}/bin/mos-mega65.cfg)
if(NOT EXISTS ${PLAIN_CFG})
  message(FATAL_ERROR "No plain platform at ${PLAIN_CFG} for the loader.")
endif()

set(LOADER_PRG ${CMAKE_CURRENT_BINARY_DIR}/simon65.prg)
add_custom_command(
  OUTPUT ${LOADER_PRG}
  COMMAND ${CMAKE_CXX_COMPILER} --config ${PLAIN_CFG}
          -Os -std=c++23 -Wall -Wextra -Werror
          -o ${LOADER_PRG} ${CMAKE_CURRENT_SOURCE_DIR}/src/loader.cpp
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/loader.cpp
  COMMENT "Building the loader")

# --- the card -----------------------------------------------------------------

# One folder holding both programs, the banks and, once extract.py has run, the
# game's data: copied to the card as it is.
set(CARD_DIR ${CMAKE_CURRENT_BINARY_DIR}/SIMON65)
add_custom_command(
  OUTPUT ${CARD_DIR}/SIMON65M.PRG
  COMMAND python3 ${MEGA65_SDK}/bin/prg-to-mega65.py
          $<TARGET_FILE:simon65mod> ${CARD_DIR} simon65m
  # An eight-and-three name, because the loader opens it through Hyppo and
  # Hyppo matches nothing longer.
  COMMAND ${CMAKE_COMMAND} -E copy ${LOADER_PRG} ${CARD_DIR}/SIMON65.PRG
  DEPENDS simon65mod ${LOADER_PRG}
  COMMENT "Building the card files")

add_custom_target(card ALL DEPENDS ${CARD_DIR}/SIMON65M.PRG)

# --- lint ---------------------------------------------------------------------
#
# The game's translation units are in the compile database; the loader is
# built by a custom command. HeaderFilterRegex carries the headers.
find_program(CLANG_TIDY_EXE NAMES clang-tidy HINTS ${LLVM_MOS}/bin)
if(CLANG_TIDY_EXE)
  set(TIDY_CMD ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/talk.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/saves.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/speech.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/room.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/frame.cpp)
  add_custom_target(tidy COMMAND ${TIDY_CMD})
else()
  message(STATUS "tidy target: skipped, no clang-tidy")
endif()

# --- release ------------------------------------------------------------------
#
# A zip a player can use without the toolchain: the programs and banks in SIMON65/,
# with extract.py, transfer.py, README and LICENSE.
set(SIMON65_VERSION "1.0.0-beta.1" CACHE STRING "the version a release zip is named for")
add_custom_target(release
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DCARD_DIR=${CARD_DIR}
          -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR} -DNAME=simon65-${SIMON65_VERSION}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/release.cmake
  DEPENDS card
  USES_TERMINAL)
