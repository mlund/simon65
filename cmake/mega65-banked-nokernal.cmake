# Toolchain for the llvm-mos SDK's mega65-banked-nokernal platform.
#
# BASIC and the KERNAL are mapped out; the code banks and the game's files come off the SD
# card through Hyppo. A toolchain file is re-read in a fresh scope for every try_compile,
# which is why the compiler is found here and not in CMakeLists.txt.
#
# LLVM_MOS and MEGA65_SDK may be given as -D options or environment variables.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mos)

if(NOT LLVM_MOS)
  if(DEFINED ENV{LLVM_MOS})
    set(LLVM_MOS "$ENV{LLVM_MOS}")
  else()
    set(LLVM_MOS "$ENV{HOME}/llvm-mos-patched2")
  endif()
endif()
if(NOT MEGA65_SDK)
  if(DEFINED ENV{MEGA65_SDK})
    set(MEGA65_SDK "$ENV{MEGA65_SDK}")
  else()
    set(MEGA65_SDK "$ENV{HOME}/llvm-mos-sdk-banked-v3")
  endif()
endif()

set(BANKED_CFG ${MEGA65_SDK}/bin/mos-mega65-banked-nokernal.cfg)
if(NOT EXISTS ${BANKED_CFG})
  message(FATAL_ERROR "No banked platform config at ${BANKED_CFG}. "
    "Set -DMEGA65_SDK=<prefix> to an SDK providing mega65-banked-nokernal.")
endif()

# The SDK's own finder: it searches LLVM_MOS, the install prefix and the PATH,
# and proves the clang it picked can actually target mos before accepting it.
include(${MEGA65_SDK}/lib/cmake/llvm-mos-sdk/find-mos-compiler.cmake)
find_mos_compiler(CMAKE_C_COMPILER mos-clang)
find_mos_compiler(CMAKE_CXX_COMPILER mos-clang++)
find_mos_compiler(CMAKE_ASM_COMPILER mos-clang)

# The config carries the platform's include and library paths, so it is needed
# when compiling and when linking.
foreach(lang C CXX ASM)
  set(CMAKE_${lang}_FLAGS_INIT "--config ${BANKED_CFG}")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "--config ${BANKED_CFG}")

set(CMAKE_FIND_ROOT_PATH ${MEGA65_SDK})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
