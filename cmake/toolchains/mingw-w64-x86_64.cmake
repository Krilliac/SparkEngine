# MinGW-w64 cross-compilation toolchain for building Windows targets on Linux.
#
# This enables compiling SparkEngine's D3D11/D3D12 code paths on a Linux host.
# The resulting .exe can be run under Wine with DXVK (D3D11->Vulkan) and
# VKD3D-Proton (D3D12->Vulkan), optionally using Lavapipe for CPU rendering.
#
# Prerequisites:
#   sudo apt-get install mingw-w64
#
# Usage:
#   cmake -B build/mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
#   cmake --build build/mingw
#
# Or use the preset:
#   cmake --preset linux-mingw-release
#   cmake --build build/linux-mingw-release

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# Cross compilers
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Target sysroot
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Search headers and libraries in the target environment only,
# but programs (e.g. protoc) in the host environment.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# MinGW linker flags — static-link libgcc/libstdc++ so the .exe runs
# under Wine without needing MinGW runtime DLLs installed.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Tell SparkEngine's CMakeLists.txt this is a MinGW cross-compilation build
set(SPARK_MINGW_CROSS ON CACHE BOOL "MinGW cross-compilation from Linux" FORCE)
