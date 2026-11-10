# Cross-build the plugin on Linux with clang-cl and lld-link against the MSVC
# CRT and Windows SDK that xwin downloads (https://github.com/Jake-Shadle/xwin).
# Made for 1.1.10-diag1, built in a Linux cloud session that had no Visual
# Studio. Releases still come from build.bat and MSVC on Windows; this is for
# test and diagnostic builds only.
#
#   cargo install xwin --locked
#   xwin --accept-license --arch x86_64 splat --output <dir>
#   cmake -S mod -B mod/build-xwin -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=mod/cmake/clang-cl-xwin.cmake -DXWIN_DIR=<dir> -DPSM_DIAG=ON \
#         [-DLLVM_BIN=/usr/lib/llvm-19/bin]
#   cmake --build mod/build-xwin
#
# XWIN_DIR is the folder given to splat --output, holding crt/ and sdk/.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
    set(XWIN_DIR "$ENV{XWIN_DIR}")
endif()
if(NOT XWIN_DIR OR NOT EXISTS "${XWIN_DIR}/crt/include")
    message(FATAL_ERROR "Set XWIN_DIR to the folder xwin splat wrote (it holds crt/ and sdk/).")
endif()
# try_compile projects run this file again and need the same folder.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR LLVM_BIN)
# xwin ships only the release CRT (no msvcrtd.lib), so CMake's compiler
# checks must link Release too.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# The LLVM tools that stand in for cl, link, lib, rc and mt. The MSVC STL
# that xwin downloads refuses clang older than 19 (error STL1000), so point
# LLVM_BIN at a clang 19+ install when the default clang-cl is older, e.g.
# -DLLVM_BIN=/usr/lib/llvm-19/bin on Ubuntu 24.04 with the clang-19 package.
if(LLVM_BIN)
    set(_llvm_hint HINTS "${LLVM_BIN}" NO_DEFAULT_PATH)
endif()
find_program(CLANG_CL NAMES clang-cl ${_llvm_hint} REQUIRED)
find_program(LLD_LINK NAMES lld-link ${_llvm_hint} REQUIRED)
find_program(LLVM_LIB NAMES llvm-lib ${_llvm_hint} REQUIRED)
find_program(LLVM_RC NAMES llvm-rc ${_llvm_hint} REQUIRED)
find_program(LLVM_MT NAMES llvm-mt ${_llvm_hint} REQUIRED)
set(CMAKE_C_COMPILER "${CLANG_CL}")
set(CMAKE_CXX_COMPILER "${CLANG_CL}")
set(CMAKE_LINKER "${LLD_LINK}")
set(CMAKE_AR "${LLVM_LIB}")
set(CMAKE_RC_COMPILER "${LLVM_RC}")
set(CMAKE_MT "${LLVM_MT}")

# The MSVC and SDK headers as system includes (/imsvc), the same set vcvars64
# puts on INCLUDE. -Wno-* silences what clang warns about in MSVC-style code
# that cl accepts, so the build output stays readable.
set(_inc "/imsvc ${XWIN_DIR}/crt/include /imsvc ${XWIN_DIR}/sdk/include/ucrt /imsvc ${XWIN_DIR}/sdk/include/um /imsvc ${XWIN_DIR}/sdk/include/shared /imsvc ${XWIN_DIR}/sdk/include/winrt")
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_inc}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_inc} -Wno-microsoft-cast -Wno-unused-command-line-argument")
# llvm-rc needs the SDK headers too, for resources.rc's #include <winres.h> etc.
set(CMAKE_RC_FLAGS_INIT "-I ${XWIN_DIR}/sdk/include/um -I ${XWIN_DIR}/sdk/include/shared -I ${XWIN_DIR}/crt/include -I ${XWIN_DIR}/sdk/include/ucrt")

# The libraries vcvars64 puts on LIB.
set(_lib "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

# Look for libraries and headers only in the Windows sysroot, never on Linux.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
