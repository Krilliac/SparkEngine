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

# Spark game modules embed the target compiler's exact C++ language level in
# their ABI metadata. Cross builds cannot execute the normal probe binary, but
# the cross-compiler itself runs on this host, so query its predefined macros.
# An explicit -DSPARK_MODULE_CXX_LANGUAGE_ABI=<value> remains authoritative.
if(NOT DEFINED SPARK_MODULE_CXX_LANGUAGE_ABI)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -dM -E -x c++ -
        RESULT_VARIABLE _spark_mingw_abi_probe_result
        OUTPUT_VARIABLE _spark_mingw_predefined_macros
        ERROR_VARIABLE _spark_mingw_abi_probe_error)
    string(REGEX MATCH "#define[ \t]+__cplusplus[ \t]+([0-9]+)L?"
        _spark_mingw_abi_match "${_spark_mingw_predefined_macros}")
    if(NOT _spark_mingw_abi_probe_result EQUAL 0 OR NOT CMAKE_MATCH_1)
        message(FATAL_ERROR
            "MinGW toolchain: failed to query target __cplusplus value: "
            "${_spark_mingw_abi_probe_error}")
    endif()
    set(SPARK_MODULE_CXX_LANGUAGE_ABI "${CMAKE_MATCH_1}" CACHE STRING
        "Target MinGW compiler __cplusplus value used by Spark module ABI checks")
    message(STATUS
        "MinGW toolchain: target C++ language ABI is ${SPARK_MODULE_CXX_LANGUAGE_ABI}")
endif()

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

# Shim directory for headers whose MinGW filename differs from (or is a
# reduced-content copy of) the Windows SDK filename. MinGW-w64 ships
# `directxmath.h` (lowercase, and only the XMFLOAT* storage types — no
# XMVECTOR/XMMATRIX/intrinsics), so engine code that `#include <DirectXMath.h>`
# would either fail (Linux case-sensitive fs) or compile against an incomplete
# header missing XMMATRIX. The fix is to add the real Microsoft DirectXMath
# headers via FetchContent (license: MIT), then prepend the downloaded Inc/
# directory AND the local cmake/mingw-shims/ directory to the include path.
#
# Offline builds can drop the Microsoft DirectXMath headers directly into
# cmake/mingw-shims/ — FetchContent will be skipped. See the README in that
# directory for details.
set(_spark_mingw_shim_dir "${CMAKE_CURRENT_LIST_DIR}/../mingw-shims")
include_directories(BEFORE SYSTEM "${_spark_mingw_shim_dir}")

if(NOT EXISTS "${_spark_mingw_shim_dir}/DirectXMath.h")
    # Headers not pre-staged — download straight to cmake/mingw-shims/ using
    # file(DOWNLOAD) + file(ARCHIVE_EXTRACT). We intentionally avoid
    # FetchContent_MakeAvailable here because this toolchain file runs during
    # CMakeSystem.cmake (before the main project() call), when add_subdirectory
    # would recursively enter another project() and fail.
    set(_dxmath_zip "${CMAKE_CURRENT_LIST_DIR}/../../build/.dxmath-cache/oct2024.zip")
    set(_dxmath_extract_dir "${CMAKE_CURRENT_LIST_DIR}/../../build/.dxmath-cache/extract")
    set(_dxmath_expected_hash "b215087ddd085381839c6fc6f51185579f3a1c55804190b2ccc5969e9520b0bd")

    if(NOT EXISTS "${_dxmath_zip}")
        message(STATUS "MinGW toolchain: downloading DirectXMath (oct2024) from Microsoft GitHub")
        file(DOWNLOAD
             "https://github.com/microsoft/DirectXMath/archive/refs/tags/oct2024.zip"
             "${_dxmath_zip}"
             EXPECTED_HASH "SHA256=${_dxmath_expected_hash}"
             STATUS _dxmath_status)
        list(GET _dxmath_status 0 _dxmath_rc)
        if(NOT _dxmath_rc EQUAL 0)
            message(WARNING "MinGW toolchain: DirectXMath download failed (${_dxmath_status}) — engine build will fail. "
                            "Drop the headers into cmake/mingw-shims/ manually, see its README.md.")
        endif()
    endif()

    if(EXISTS "${_dxmath_zip}" AND NOT EXISTS "${_dxmath_extract_dir}/DirectXMath-oct2024/Inc/DirectXMath.h")
        file(MAKE_DIRECTORY "${_dxmath_extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_dxmath_zip}" DESTINATION "${_dxmath_extract_dir}")
    endif()

    if(EXISTS "${_dxmath_extract_dir}/DirectXMath-oct2024/Inc/DirectXMath.h")
        include_directories(BEFORE SYSTEM "${_dxmath_extract_dir}/DirectXMath-oct2024/Inc")
        message(STATUS "MinGW toolchain: using fetched DirectXMath headers from "
                       "${_dxmath_extract_dir}/DirectXMath-oct2024/Inc")
    endif()
else()
    message(STATUS "MinGW toolchain: using local DirectXMath headers from ${_spark_mingw_shim_dir}")
endif()
