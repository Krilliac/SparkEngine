cmake_minimum_required(VERSION 3.25)

# ================================================================
# Modern CMake Configuration - Cross-Platform Build
# ================================================================

# All targets need PIC since game modules are shared libraries that link SparkEngineLib
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Set CMake policies for modern behavior
cmake_policy(SET CMP0091 NEW)  # Enable MSVC_RUNTIME_LIBRARY support
cmake_policy(SET CMP0141 NEW)  # Select MSVC debug-information format through CMake

# Note: CMAKE_MSVC_RUNTIME_LIBRARY is set after project() below,
# because the MSVC variable is not defined until the compiler is identified.

# MSVC toolset selection:
#   v143 = Visual Studio 2022
#   v145 = Visual Studio 2026
# Prefer the generator's -T option (or a preset's "toolset" field).
set(SPARK_MSVC_TOOLSET "" CACHE STRING
    "Deprecated compatibility hint for the selected MSVC toolset. Use cmake -T instead.")

# ---------------------------------------------------------------------
# 1) Disable all third-party tests/examples globally
# ---------------------------------------------------------------------
set(BUILD_TESTING OFF CACHE BOOL "Disable all testing" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable all examples" FORCE)
set(BUILD_SAMPLES OFF CACHE BOOL "Disable all samples" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "Disable all tools" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "Disable docs" FORCE)

# ---------------------------------------------------------------------
# 2) Global generator and project settings
# ---------------------------------------------------------------------
# Generator toolset/platform are immutable generator inputs. Never rewrite
# them from project code; use -T/-A or CMakePresets.json instead.
if(WIN32 AND NOT SPARK_MSVC_TOOLSET STREQUAL "")
    if(CMAKE_GENERATOR_TOOLSET AND NOT CMAKE_GENERATOR_TOOLSET STREQUAL SPARK_MSVC_TOOLSET)
        message(FATAL_ERROR
            "SPARK_MSVC_TOOLSET=${SPARK_MSVC_TOOLSET} disagrees with generator toolset "
            "${CMAKE_GENERATOR_TOOLSET}. Configure with -T ${SPARK_MSVC_TOOLSET} instead.")
    elseif(NOT CMAKE_GENERATOR_TOOLSET)
        message(WARNING
            "SPARK_MSVC_TOOLSET is a compatibility hint only. Select the toolset with "
            "-T ${SPARK_MSVC_TOOLSET} or a preset toolset field.")
    endif()
endif()

# macOS deployment target — must be set before project() so the compiler is
# invoked with the correct -mmacosx-version-min. 11.0 (Big Sur) is the floor:
# first Apple Silicon release, first to ship with Metal 3 on M-series GPUs,
# and the minimum SDL2 version shipped via Homebrew still supports it.
# Override with -DCMAKE_OSX_DEPLOYMENT_TARGET=X.Y at configure time.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(SPARK_ENGINE_VERSION "1.0.0" CACHE STRING "SparkEngine semantic version used for package metadata")

# Objective-C / Objective-C++ are required on macOS so SDL2's cocoa/.m sources
# (SDL_cocoaevents.m, SDL_cocoametalview.m, SDL_rwopsbundlesupport.m, etc.) and
# our own MetalDevice.mm can compile. Without them CMake silently skips .m/.mm
# sources and the link fails with "no such file or directory" on the .m.o files.
if(APPLE)
    project(SparkEngine VERSION ${SPARK_ENGINE_VERSION} LANGUAGES CXX C OBJC OBJCXX)
else()
    project(SparkEngine VERSION ${SPARK_ENGINE_VERSION} LANGUAGES CXX C)
endif()

include("${CMAKE_SOURCE_DIR}/cmake/SparkWindowsVersionInfo.cmake")

# Keep the public SDK's semantic version in lockstep with project()/CPack.
# Version.h retains the independently managed SDK ABI number and includes this
# generated header for the release version macros.
set(SPARK_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${SPARK_GENERATED_INCLUDE_DIR}/Spark")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/SparkGeneratedVersion.h.in"
    "${SPARK_GENERATED_INCLUDE_DIR}/Spark/GeneratedVersion.h"
    @ONLY
)

# Set consistent runtime library for all targets using modern CMake (CMP0091)
# Must be after project() so that the MSVC variable is defined.
if(MSVC)
    if(MSVC_VERSION LESS 1936)
        message(FATAL_ERROR "SparkEngine requires MSVC 19.36 or newer (Visual Studio 2022 17.6+)")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # /Z7 embeds CodeView data in object files, allowing sccache to cache
    # Debug builds while link-time /DEBUG still emits PDBs.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
endif()

# ---------------------------------------------------------------------
# 2.5) Compiler identification and version reporting
# ---------------------------------------------------------------------
if(MSVC)
    # Map _MSC_VER ranges to Visual Studio release years
    if(MSVC_VERSION GREATER_EQUAL 1950)
        set(SPARK_VS_YEAR "2026+")
        set(SPARK_VS_TOOLSET "v145+")
    elseif(MSVC_VERSION GREATER_EQUAL 1930)
        set(SPARK_VS_YEAR "2022")
        set(SPARK_VS_TOOLSET "v143")
    elseif(MSVC_VERSION GREATER_EQUAL 1920)
        set(SPARK_VS_YEAR "2019")
        set(SPARK_VS_TOOLSET "v142")
    elseif(MSVC_VERSION GREATER_EQUAL 1910)
        set(SPARK_VS_YEAR "2017")
        set(SPARK_VS_TOOLSET "v141")
    else()
        set(SPARK_VS_YEAR "2015 or older")
        set(SPARK_VS_TOOLSET "v14x")
    endif()
    message(STATUS "Compiler : MSVC Visual Studio ${SPARK_VS_YEAR} (${SPARK_VS_TOOLSET}, _MSC_VER=${MSVC_VERSION})")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(STATUS "Compiler : Apple Clang ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    message(STATUS "Compiler : Intel oneAPI DPC++/C++ (ICPX) ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
    message(STATUS "Compiler : Intel C++ Classic (ICC) ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Compiler : LLVM Clang ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "Compiler : GCC ${CMAKE_CXX_COMPILER_VERSION}")
else()
    message(STATUS "Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

# ---------------------------------------------------------------------
# 3) Custom build options
# ---------------------------------------------------------------------
option(ENABLE_GRAPHICS "Enable graphics engine" ON)
option(ENABLE_PROFILING "Enable profiling tools" ON)
option(ENABLE_NETWORKING "Enable networking features (UDP sockets)" ON)
option(ENABLE_VULKAN "Enable Vulkan graphics backend (experimental)" ON)
option(ENABLE_OPENGL "Enable OpenGL graphics backend (experimental)" ON)
option(ENABLE_METAL "Enable Metal graphics backend (macOS, experimental)" ${APPLE})
option(ENABLE_DXR "Enable DirectX Raytracing support (requires D3D12, runtime fallback to SDFGI)" ON)
option(ENABLE_HYBRID_RT "Enable hybrid ray tracing (SDFGI software + optional hardware DXR/Vulkan RT)" ON)
# SDL2 is auto-enabled on Linux for windowing + input; can be used on Windows too
if(NOT WIN32)
    option(ENABLE_SDL2 "Enable SDL2 cross-platform input and windowing" ON)
else()
    option(ENABLE_SDL2 "Enable SDL2 cross-platform input and windowing" OFF)
endif()
option(SPARK_HEADLESS_SUPPORT "Enable headless/dedicated server mode support" ON)
option(ENABLE_ANGELSCRIPT "Enable the AngelScript gameplay scripting runtime" ON)
option(ENABLE_RECAST "Enable Recast/Detour navmesh generation (improved voxel-based builder)" ON)
option(ENABLE_NEURAL_RENDERING "Enable neural rendering features (NTC, radiance cache, neural post-processing)" ON)
option(ENABLE_VR "Enable VR/AR framework (OpenXR-ready stub)" OFF)
option(ENABLE_MOBILE "Enable mobile platform support (touch, gestures, battery-aware scaling)" OFF)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_GAME_MODULES "Build in-tree game modules (SparkGameFPS, SparkGameMMO, etc.)" ON)
option(SPARK_STRICT_DEPS "FATAL_ERROR on missing critical dependencies (Jolt, ImGui, EnTT)" OFF)
option(SPARK_SUPPRESS_THIRDPARTY_WARNINGS "Suppress compiler warnings from third-party libraries (Jolt, etc.)" ON)
option(SPARK_DOUBLE_PRECISION_PHYSICS "Enable double precision physics (JPH_DOUBLE_PRECISION) for large worlds" OFF)

# Editor available on Windows (Win32+DX11) and Linux (SDL2+OpenGL3)
option(ENABLE_EDITOR "Enable editor features" ON)
option(ENABLE_LAUNCHER "Build SparkLauncher (Unity-Hub-style project picker)" ON)
option(ENABLE_SPARKBUILD "Build SparkBuild (terminal-UI CMake configurator)" ON)
option(ENABLE_INSTALLER "Build SparkInstaller (bootstrap installer/updater)" ON)
option(ENABLE_SERVER_PROCESSES "Build dedicated server, gateway, and orchestration processes" ON)
option(ENABLE_ASSET_PIPELINE_TOOLS "Build deterministic cooker and worker processes" ON)
option(ENABLE_AUTOMATION_HOST "Build the standalone automation host" ON)

# Build profile options — controls what ships in the final binary
option(ENABLE_CONSOLE_IN_SHIPPING "Include SparkConsole in Shipping builds" OFF)
option(ENABLE_DEVCOMMANDS_IN_SHIPPING "Include developer commands (god, noclip, etc.) in Shipping builds" OFF)
option(STRIP_DEBUG_SYMBOLS "Strip debug symbols from the final binary" OFF)

# Third-party manifest audit (single source of truth lives in ThirdParty/dependencies.lock)
include("${CMAKE_SOURCE_DIR}/cmake/SparkThirdPartyAudit.cmake")
spark_thirdparty_audit("${CMAKE_SOURCE_DIR}/ThirdParty/dependencies.lock")
set(SPARK_THIRD_PARTY_NOTICES_FILE "${CMAKE_BINARY_DIR}/SparkEngineThirdPartyNotices.txt")
spark_thirdparty_generate_notice(
    "${CMAKE_SOURCE_DIR}/ThirdParty/dependencies.lock"
    "${SPARK_THIRD_PARTY_NOTICES_FILE}")

# ---------------------------------------------------------------------
# 4) C++ standard and in-source build guard
# ---------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Create a separate build directory.")
endif()

# ---------------------------------------------------------------------
# 5) Output directories - ENSURE BOTH EXECUTABLES GO TO SAME DIRECTORY
# ---------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Multi-config generators must keep every artifact configuration-specific.
# A flat runtime directory lets Debug overwrite a Release DLL/executable while
# CMake still considers the Release target up to date. The next Release build
# can then pair that stale Debug binary with newly generated Release metadata
# (observed with module ABI sidecars), or launch the wrong configuration.
# Single-config Ninja/Make builds retain the established flat bin/ and lib/
# layout used by Linux/macOS tooling and CI.
get_property(SPARK_GENERATOR_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    if(SPARK_GENERATOR_IS_MULTI_CONFIG)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin/${config})
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib/${config})
    else()
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
    endif()
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib/${config})
endforeach()

# ---------------------------------------------------------------------
# 6) Compiler options (platform-specific)
# ---------------------------------------------------------------------
if(MSVC)
    # Note: /GS (buffer security check) is enabled by default in MSVC — no explicit flag needed.
    add_compile_options(
        /W3 /MP /bigobj
        /wd4005  # Macro redefinition
        /wd4996  # Deprecated functions
        /wd4244  # Conversion warnings
        /wd4267  # size_t conversion warnings
        /wd26495 # Uninitialized member variable warnings (for TinyObjLoader)
    )
    # /DEBUG linker flag embeds PDB reference in the executable so SymFromAddr
    # can locate symbols at runtime. Does not affect binary performance.
    add_link_options(/DEBUG)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS)

    # Conformance improvements available in VS 2017+ — enforce __cplusplus macro and
    # disable non-conforming extensions so code is portable across compilers.
    if(MSVC_VERSION GREATER_EQUAL 1910)
        add_compile_options(/Zc:__cplusplus /permissive-)
    endif()

    # VS 2026 (v145, _MSC_VER >= 1950): enable C++26 readiness flags
    if(MSVC_VERSION GREATER_EQUAL 1950)
        add_compile_options(
            /Zc:preprocessor   # Standard-conforming preprocessor
            /wd5045            # Spectre mitigation informational warning
        )
        add_compile_definitions(SPARK_COMPILER_VS2026)
    # VS 2022 (v143, _MSC_VER 1930-1944)
    elseif(MSVC_VERSION GREATER_EQUAL 1930)
        add_compile_options(/Zc:preprocessor)
        add_compile_definitions(SPARK_COMPILER_VS2022)
    # VS 2019 (v142, _MSC_VER 1920-1929)
    elseif(MSVC_VERSION GREATER_EQUAL 1920)
        add_compile_definitions(SPARK_COMPILER_VS2019)
    endif()

elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM")
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-sign-compare
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-reorder
        -Wno-ignored-qualifiers
        -fno-omit-frame-pointer  # Preserve frame pointers so backtrace() can walk the full call stack
        -fstack-protector-strong # Stack buffer overflow detection (guards functions with local arrays/alloca)
    )

    # FORTIFY_SOURCE — compile-time and runtime buffer overflow checks for libc functions.
    # Requires -O1 or higher, so only enable for optimized builds.
    add_compile_definitions(
        $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>
    )

    # Full RELRO — mark the GOT read-only after relocation (Linux ELF only).
    # Prevents GOT overwrite attacks. -z,now forces immediate binding (no lazy PLT).
    # Skip on MinGW / Windows: PE linkers don't understand -z and would fail.
    if(NOT APPLE AND NOT WIN32 AND NOT MINGW)
        add_link_options(-Wl,-z,relro,-z,now)
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(
            -Wno-class-memaccess
            -Wno-stringop-truncation
        )
    endif()
    # Clang (LLVM), Apple Clang, and Intel LLVM (ICPX) share Clang-compatible flags
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|IntelLLVM")
        add_compile_options(
            -Wno-unused-private-field
            -Wno-unused-lambda-capture
        )
        # -Wno-microsoft-include only relevant on Windows with LLVM/Clang
        if(WIN32)
            add_compile_options(-Wno-microsoft-include)
        endif()
    endif()
    # Intel LLVM (ICPX) extra suppressions
    if(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
        add_compile_options(-Wno-tautological-constant-compare)
    endif()
endif()

# ---------------------------------------------------------------------
# 6.5) Per-configuration optimization flags
#
# CMake defaults are minimal:
#   Debug       → -O0 -g           (MSVC: /Od /Zi)
#   Release     → -O3 -DNDEBUG     (MSVC: /O2 /DNDEBUG)
#   RelWithDebInfo → -O2 -g -DNDEBUG
#   MinSizeRel  → -Os -DNDEBUG     (MSVC: /O1 /DNDEBUG)
#
# We add engine-specific flags on top:
#   - Release/Shipping: LTO, intrinsics, vectorization, linker GC
#   - Debug: -Og (GCC), runtime checks, fast linking
# ---------------------------------------------------------------------

# --- Option: native architecture tuning (OFF for distributed builds) ---
option(SPARK_NATIVE_ARCH "Tune for the build machine's CPU (SSE4.2/AVX2/etc). Disable for distributed binaries." OFF)

if(MSVC)
    # ── Release / RelWithDebInfo / MinSizeRel (all optimized configs) ──
    add_compile_options(
        # Intrinsics — let the compiler use hardware instructions (sqrt, memcpy, etc.)
        $<$<NOT:$<CONFIG:Debug>>:/Oi>
        # Favor speed over size in Release and RelWithDebInfo
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/O2>
        # Shipping (MinSizeRel) also gets speed — /O2 is better than /O1 for games
        $<$<CONFIG:MinSizeRel>:/O2>
        # Whole-program optimization (enables cross-TU inlining via LTCG at link time)
        $<$<NOT:$<CONFIG:Debug>>:/GL>
        # Favor 64-bit code generation (branch prediction, register allocation)
        $<$<NOT:$<CONFIG:Debug>>:/favor:AMD64>
        # String pooling — merge identical string literals
        $<$<NOT:$<CONFIG:Debug>>:/GF>
        # Function-level linking — enables linker dead-code elimination (/OPT:REF)
        $<$<NOT:$<CONFIG:Debug>>:/Gy>
    )

    # ── Release / Shipping linker optimizations ──
    add_link_options(
        # Link-Time Code Generation — cross-TU inlining, devirtualization (pairs with /GL)
        $<$<NOT:$<CONFIG:Debug>>:/LTCG>
        # Remove unreferenced functions/data (dead code elimination)
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
        # Fold identical COMDATs (identical functions/constants merged)
        $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
    )

    # ── Debug ──
    add_compile_options(
        # Runtime error checks: stack frame validation + uninitialized variable detection
        $<$<CONFIG:Debug>:/RTC1>
        # Just My Code debugging — skip stepping into framework/library code
        $<$<CONFIG:Debug>:/JMC>
    )
    # Incremental linking for faster debug iteration
    add_link_options(
        $<$<CONFIG:Debug>:/INCREMENTAL>
    )

    # MSVC iterator debug level: level 1 in Debug gives bounds checking without
    # the extreme slowdown of level 2 (which is MSVC's default for Debug)
    add_compile_definitions(
        $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=1>
    )

elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM")
    # ── Release / RelWithDebInfo / MinSizeRel ──
    add_compile_options(
        # Split functions/data into individual sections for linker GC
        $<$<NOT:$<CONFIG:Debug>>:-ffunction-sections>
        $<$<NOT:$<CONFIG:Debug>>:-fdata-sections>
        # Strict aliasing — enables type-based alias analysis optimizations
        $<$<NOT:$<CONFIG:Debug>>:-fstrict-aliasing>
    )

    # LTO for Release and MinSizeRel (not RelWithDebInfo — it bloats debug info)
    option(ENABLE_LTO "Enable link-time optimization in Release/MinSizeRel builds" ON)
    if(ENABLE_LTO)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(
                $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-flto=auto>
            )
            add_link_options(
                $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-flto=auto>
            )
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|IntelLLVM")
            add_compile_options(
                $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-flto=thin>
            )
            add_link_options(
                $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-flto=thin>
            )
        endif()
    endif()

    # Linker dead-code elimination (pairs with -ffunction-sections/-fdata-sections)
    if(NOT APPLE)
        add_link_options(
            $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
        )
    else()
        add_link_options(
            $<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>
        )
    endif()

    # ── Shipping (MinSizeRel): override -Os with -O3 for speed ──
    # CMake sets -Os for MinSizeRel by default; games need speed, not size.
    # We override at the target level after CMake's defaults are set.
    string(REPLACE "-Os" "-O3" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
    string(REPLACE "-Os" "-O3" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")

    # ── Debug: -Og instead of -O0 ──
    # -Og is "optimize for debugging" — ~2x faster than -O0 while preserving
    # almost all debug information. Variables are less likely to be optimized out
    # compared to -O1/-O2, making it ideal for stepping through code.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        string(REPLACE "-O0" "-Og" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "-O0" "-Og" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    endif()

    # Debug: GCC assertions for STL containers (bounds checking on operator[], iterators)
    add_compile_definitions(
        $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
    )

    # Shipping: strip symbols from the final binary
    if(NOT APPLE)
        add_link_options(
            $<$<CONFIG:MinSizeRel>:-Wl,--strip-all>
        )
    else()
        add_link_options(
            $<$<CONFIG:MinSizeRel>:-Wl,-S>
        )
    endif()
endif()

# ── Architecture tuning (opt-in, all compilers) ──
if(SPARK_NATIVE_ARCH)
    if(MSVC)
        # MSVC doesn't have -march=native; enable AVX2 which covers most modern CPUs
        add_compile_options($<$<NOT:$<CONFIG:Debug>>:/arch:AVX2>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM")
        add_compile_options($<$<NOT:$<CONFIG:Debug>>:-march=native>)
    endif()
    message(STATUS "Native architecture tuning: ENABLED (not suitable for distribution)")
else()
    # Baseline: SSE4.2 is universally available on x86-64 CPUs since ~2010
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        if(NOT MSVC)
            add_compile_options($<$<NOT:$<CONFIG:Debug>>:-msse4.2>)
        endif()
    endif()
endif()

# MinGW cross-compilation from Linux: defines the Windows macros needed by the
# general Windows and D3D11 paths. The D3D12/DXR sources are excluded below
# because the MinGW SDK headers do not provide the required interfaces.
if(MINGW AND NOT MSVC)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX SPARK_PLATFORM_WINDOWS)
    message(STATUS "MinGW detected — enabling Windows/D3D11 code paths; D3D12/DXR will be excluded")

    # MinGW toolchain restricts find_path to the sysroot. Our ThirdParty
    # dependencies live in the source tree, so allow both search modes.
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
endif()

# ---------------------------------------------------------------------
# 7) Third-party library configuration
# ---------------------------------------------------------------------

# SDL2 - Built from submodule on Linux (before anything else needs it)
set(SDL2_SUBMODULE_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/SDL2")
if(NOT WIN32 AND ENABLE_SDL2 AND EXISTS "${SDL2_SUBMODULE_DIR}/CMakeLists.txt")
    # Probe for OpenGL first — SDL2 auto-detects GLX when headers are present.
    # Without libgl-dev, SDL2 still builds but without OpenGL support.
    find_package(OpenGL QUIET)
    if(NOT OpenGL_FOUND)
        # Fallback: if libGL.so.1 exists (Mesa runtime) but libgl-dev is not installed,
        # use our bundled minimal GL/GLX headers and create the libGL.so symlink.
        # This lets SDL2 detect and compile with OpenGL/GLX support even without the
        # -dev package, which is common in CI/container environments.
        set(_FALLBACK_GL_HEADERS "${CMAKE_SOURCE_DIR}/ThirdParty/OpenGL")
        find_library(_LIBGL_RUNTIME NAMES GL PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64 NO_DEFAULT_PATH)
        if(NOT _LIBGL_RUNTIME)
            # Also check the versioned name directly
            find_file(_LIBGL_SO1 NAMES libGL.so.1 PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64 NO_DEFAULT_PATH)
            if(_LIBGL_SO1)
                set(_LIBGL_RUNTIME "${_LIBGL_SO1}")
            endif()
        endif()

        if(_LIBGL_RUNTIME AND EXISTS "${_FALLBACK_GL_HEADERS}/GL/gl.h")
            message(WARNING
                "OpenGL dev headers not found — using bundled fallback (ThirdParty/OpenGL).\n"
                "  Runtime libGL found at: ${_LIBGL_RUNTIME}\n"
                "  Install libgl-dev (or equivalent) for full system OpenGL headers.")

            # Create libGL.so symlink if it doesn't exist (needs the -dev package normally)
            get_filename_component(_GL_LIB_DIR "${_LIBGL_RUNTIME}" DIRECTORY)
            if(NOT EXISTS "${_GL_LIB_DIR}/libGL.so")
                execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "${_LIBGL_RUNTIME}" "${_GL_LIB_DIR}/libGL.so"
                    RESULT_VARIABLE _SYMLINK_RESULT)
                if(_SYMLINK_RESULT EQUAL 0)
                    message(STATUS "  Created dev symlink: ${_GL_LIB_DIR}/libGL.so -> ${_LIBGL_RUNTIME}")
                else()
                    message(WARNING "  Could not create libGL.so dev symlink (may need sudo) — linking may fail")
                endif()
            endif()

            # Also create libGLX.so symlink if GLX runtime exists without dev symlink
            find_file(_LIBGLX_SO0 NAMES libGLX.so.0 PATHS "${_GL_LIB_DIR}" NO_DEFAULT_PATH)
            if(_LIBGLX_SO0 AND NOT EXISTS "${_GL_LIB_DIR}/libGLX.so")
                execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "${_LIBGLX_SO0}" "${_GL_LIB_DIR}/libGLX.so"
                    RESULT_VARIABLE _GLX_SYMLINK_RESULT)
                if(_GLX_SYMLINK_RESULT EQUAL 0)
                    message(STATUS "  Created symlink: ${_GL_LIB_DIR}/libGLX.so -> ${_LIBGLX_SO0}")
                endif()
            endif()

            # Inject fallback headers so SDL2's CheckOpenGL and the engine can find them.
            # CMAKE_C_FLAGS -isystem is needed because SDL2's check_c_source_compiles
            # doesn't inherit include_directories from the parent project.
            list(APPEND CMAKE_INCLUDE_PATH "${_FALLBACK_GL_HEADERS}")
            include_directories(SYSTEM "${_FALLBACK_GL_HEADERS}")
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isystem ${_FALLBACK_GL_HEADERS}" CACHE STRING "" FORCE)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isystem ${_FALLBACK_GL_HEADERS}" CACHE STRING "" FORCE)
            set(OPENGL_INCLUDE_DIR "${_FALLBACK_GL_HEADERS}" CACHE PATH "Fallback GL headers" FORCE)
            set(OPENGL_gl_LIBRARY "${_GL_LIB_DIR}/libGL.so" CACHE FILEPATH "GL library" FORCE)
            set(OpenGL_FOUND TRUE)
            set(OPENGL_FOUND TRUE)
            set(SPARK_GL_FALLBACK_HEADERS TRUE)

            # Create the OpenGL::GL imported target that CMake's FindOpenGL normally provides.
            # Without this, target_link_libraries(... OpenGL::GL) generates -lOpenGL::GL literally.
            if(NOT TARGET OpenGL::GL)
                add_library(OpenGL::GL UNKNOWN IMPORTED)
                set_target_properties(OpenGL::GL PROPERTIES
                    IMPORTED_LOCATION "${_GL_LIB_DIR}/libGL.so"
                    INTERFACE_INCLUDE_DIRECTORIES "${_FALLBACK_GL_HEADERS}")
            endif()
        else()
            message(WARNING
                "OpenGL headers not found — SDL2 will build WITHOUT OpenGL/GLX support.\n"
                "  The editor requires OpenGL. Install the development headers:\n"
                "    Ubuntu/Debian : sudo apt install libgl-dev\n"
                "    Fedora        : sudo dnf install mesa-libGL-devel\n"
                "    Arch          : sudo pacman -S mesa\n"
                "  Then delete the build directory and reconfigure.")
        endif()
    endif()

    # Minimal SDL2 build — only what the engine needs
    set(SDL_SHARED   ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC   OFF CACHE BOOL "" FORCE)
    set(SDL_TEST     OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGL   ON  CACHE BOOL "" FORCE)
    # SparkEngine owns the component-aware SDL install rules below. Disable
    # SDL's generic no-component rules so CPack never installs the same DLL or
    # import library twice and component-only installs remain self-contained.
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL
        "SparkEngine provides component-aware SDL2 install rules" FORCE)

    if(APPLE)
        # macOS: Cocoa + CoreAudio. X11/Wayland/ALSA etc. are irrelevant.
        set(SDL_X11      OFF CACHE BOOL "" FORCE)
        set(SDL_WAYLAND  OFF CACHE BOOL "" FORCE)
        set(SDL_KMSDRM   OFF CACHE BOOL "" FORCE)
        set(SDL_DBUS     OFF CACHE BOOL "" FORCE)
        set(SDL_IBUS     OFF CACHE BOOL "" FORCE)
        set(SDL_PULSEAUDIO OFF CACHE BOOL "" FORCE)
        set(SDL_ALSA     OFF CACHE BOOL "" FORCE)
        set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
        set(SDL_SNDIO    OFF CACHE BOOL "" FORCE)
        set(SDL_JACK     OFF CACHE BOOL "" FORCE)
    else()
        # Linux: X11 + OpenGL, everything else off to keep the build lean.
        set(SDL_X11      ON  CACHE BOOL "" FORCE)
        set(SDL_WAYLAND  OFF CACHE BOOL "" FORCE)
        set(SDL_KMSDRM   OFF CACHE BOOL "" FORCE)
        set(SDL_DBUS     OFF CACHE BOOL "" FORCE)
        set(SDL_IBUS     OFF CACHE BOOL "" FORCE)
        set(SDL_PULSEAUDIO OFF CACHE BOOL "" FORCE)
        set(SDL_ALSA     OFF CACHE BOOL "" FORCE)
        set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
        set(SDL_SNDIO    OFF CACHE BOOL "" FORCE)
        set(SDL_JACK     OFF CACHE BOOL "" FORCE)
    endif()

    add_subdirectory("${SDL2_SUBMODULE_DIR}" "${CMAKE_BINARY_DIR}/ThirdParty/SDL2" EXCLUDE_FROM_ALL)
    set(SDL2_FOUND TRUE)

    # Eagerly copy SDL2's public headers to the build tree at configure time.
    # SDL2 normally copies them via a build-time custom_target ("sdl_headers_copy"),
    # but consumers that pick up SDL2's INTERFACE_INCLUDE_DIRECTORIES without a
    # direct build-order dep on SDL2 (e.g. cached CI restores, tests using <SDL.h>
    # through SparkEngineLib's PUBLIC include path) can race the copy and fail
    # with "fatal error: SDL_main.h: No such file or directory" at
    # build/ThirdParty/SDL2/include/SDL2/SDL.h. Copying at configure time makes
    # the headers available before any compilation job runs.
    file(GLOB _SDL2_PUBLIC_HEADERS "${SDL2_SUBMODULE_DIR}/include/*.h")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ThirdParty/SDL2/include/SDL2")
    foreach(_hdr IN LISTS _SDL2_PUBLIC_HEADERS)
        if(NOT _hdr MATCHES ".*(SDL_config|SDL_revision).*")
            get_filename_component(_name "${_hdr}" NAME)
            configure_file("${_hdr}"
                "${CMAKE_BINARY_DIR}/ThirdParty/SDL2/include/SDL2/${_name}"
                COPYONLY)
        endif()
    endforeach()

    message(STATUS "SDL2 built from submodule (ThirdParty/SDL2)")
endif()

# Single source of truth for third-party target creation:
# dependencies are created in this root CMakeLists.txt and consumed elsewhere.
# MINIZ - Simple compression library
set(MINIZ_ROOT "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/miniz")
if(EXISTS "${MINIZ_ROOT}")
    file(GLOB_RECURSE MINIZ_SOURCES
        "${MINIZ_ROOT}/*.c"
        "${MINIZ_ROOT}/*.h"
    )
    list(FILTER MINIZ_SOURCES EXCLUDE REGEX ".*test.*|.*example.*|.*tool.*")
endif()

if(MINIZ_SOURCES)
    # Create miniz library target
    add_library(miniz STATIC ${MINIZ_SOURCES})
    target_include_directories(miniz PUBLIC
        $<BUILD_INTERFACE:${MINIZ_ROOT}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/miniz>
    )

    # Generate miniz_export.h (required by miniz headers, empty for static builds)
    include(GenerateExportHeader)
    generate_export_header(miniz)

    # Set consistent runtime library for miniz
    if(MSVC)
        set_property(TARGET miniz PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    set(MINIZ_FOUND TRUE)
    message(STATUS "Found MINIZ at: ${MINIZ_ROOT}")
else()
    set(MINIZ_FOUND FALSE)
    message(STATUS "MINIZ not found (directory empty or missing)")
endif()

# ============================================================================
# zstd — Zstandard compression library (faster decompression than miniz)
# ============================================================================
set(ZSTD_ROOT "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/zstd")
if(NOT TARGET zstd AND EXISTS "${ZSTD_ROOT}/zstd.h")
    add_library(zstd STATIC "${ZSTD_ROOT}/zstd_impl.cpp")
    target_include_directories(zstd PUBLIC
        $<BUILD_INTERFACE:${ZSTD_ROOT}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/zstd>
    )
    if(MSVC)
        target_compile_options(zstd PRIVATE /W0)
        set_property(TARGET zstd PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(zstd PRIVATE -w)
    endif()
    message(STATUS "Found zstd at: ${ZSTD_ROOT}")
else()
    message(STATUS "zstd not found (directory empty or missing)")
endif()

# ============================================================================
# VMA — Vulkan Memory Allocator (header-only, AMD GPUOpen)
# ============================================================================
set(VMA_ROOT "${CMAKE_SOURCE_DIR}/ThirdParty/VulkanMemoryAllocator")
if(NOT TARGET VulkanMemoryAllocator AND EXISTS "${VMA_ROOT}/vk_mem_alloc.h")
    add_library(VulkanMemoryAllocator INTERFACE)
    target_include_directories(VulkanMemoryAllocator INTERFACE "${VMA_ROOT}")
    message(STATUS "Found VMA at: ${VMA_ROOT}")
else()
    message(STATUS "VMA not found (directory missing)")
endif()

# TinyObjLoader - Proper static library implementation
find_path(TINYOBJLOADER_INCLUDE_DIR
    NAMES tiny_obj_loader.h
    PATHS
        "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/tinyobjloader"
        "${CMAKE_SOURCE_DIR}/ThirdParty/FileFormats/tinyobjloader"
        "${CMAKE_SOURCE_DIR}/ThirdParty/Graphics/tinyobjloader"
        "${CMAKE_SOURCE_DIR}/ThirdParty/tinyobjloader"
        "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/tinyobjloader/include"
        "${CMAKE_SOURCE_DIR}/ThirdParty/FileFormats/tinyobjloader/include"
    DOC "TinyObjLoader include directory"
)

if(TINYOBJLOADER_INCLUDE_DIR)
    # Create a proper static library for TinyObjLoader instead of INTERFACE
    # This prevents duplicate symbol issues by having only one implementation
    add_library(tinyobjloader STATIC
        "SparkEngine/Source/Utils/TinyObjImpl.cpp"
    )
    target_include_directories(tinyobjloader PUBLIC
        $<BUILD_INTERFACE:${TINYOBJLOADER_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/tinyobjloader>
    )
    # Don't add TINYOBJLOADER_IMPLEMENTATION to the interface - it's only in TinyObjImpl.cpp

    # Set consistent runtime library for tinyobjloader
    if(MSVC)
        set_property(TARGET tinyobjloader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    set(TINYOBJ_FOUND TRUE)
    message(STATUS "Found TinyObjLoader at: ${TINYOBJLOADER_INCLUDE_DIR}")
    message(STATUS "TinyObjLoader implementation will be compiled once in TinyObjImpl.cpp")
else()
    set(TINYOBJ_FOUND FALSE)
    message(STATUS "TinyObjLoader not found (optional)")
endif()

# =========================================================================
# stb libraries (stb_image, stb_image_write) - Public Domain image I/O
# =========================================================================
set(STB_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/stb")
if(NOT TARGET stb_image AND EXISTS "${STB_DIR}/stb_image.h")
    add_library(stb_image STATIC "${STB_DIR}/stb_impl.cpp")
    target_include_directories(stb_image PUBLIC
        $<BUILD_INTERFACE:${STB_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/stb>
    )
    if(MSVC)
        target_compile_options(stb_image PRIVATE /W0)
        set_property(TARGET stb_image PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(stb_image PRIVATE -w)
    endif()
    message(STATUS "Found stb_image (PNG/JPG/BMP/TGA/HDR)")
endif()

# =========================================================================
# cgltf - single-file glTF 2.0 parser (MIT)
# =========================================================================
set(CGLTF_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/cgltf")
if(NOT TARGET cgltf AND EXISTS "${CGLTF_DIR}/cgltf.h")
    add_library(cgltf STATIC "${CGLTF_DIR}/cgltf_impl.cpp")
    target_include_directories(cgltf PUBLIC
        $<BUILD_INTERFACE:${CGLTF_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/cgltf>
    )
    if(MSVC)
        target_compile_options(cgltf PRIVATE /W0)
        set_property(TARGET cgltf PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(cgltf PRIVATE -w)
    endif()
    message(STATUS "Found cgltf (glTF 2.0 parser)")
endif()

# =========================================================================
# miniaudio - cross-platform audio (Public Domain / MIT-0)
# =========================================================================
set(MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Audio/miniaudio")
if(NOT TARGET miniaudio AND EXISTS "${MINIAUDIO_DIR}/miniaudio.h")
    add_library(miniaudio STATIC "${MINIAUDIO_DIR}/miniaudio_impl.cpp")
    target_include_directories(miniaudio PUBLIC
        $<BUILD_INTERFACE:${MINIAUDIO_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/miniaudio>
    )
    if(MSVC)
        target_compile_options(miniaudio PRIVATE /W0)
        set_property(TARGET miniaudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(miniaudio PRIVATE -w)
    endif()
    if(NOT WIN32 AND NOT APPLE)
        find_package(Threads QUIET)
        if(Threads_FOUND)
            target_link_libraries(miniaudio PRIVATE Threads::Threads)
        endif()
        target_link_libraries(miniaudio PRIVATE ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "Found miniaudio (cross-platform audio)")
endif()

# =========================================================================
# nlohmann/json - JSON for Modern C++ (MIT, header-only)
# =========================================================================
set(NLOHMANN_JSON_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/json")
if(NOT TARGET nlohmann_json AND EXISTS "${NLOHMANN_JSON_DIR}/nlohmann_json.h")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE
        $<BUILD_INTERFACE:${NLOHMANN_JSON_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/json>
    )
    message(STATUS "Found nlohmann/json (header-only)")
endif()

# =========================================================================
# tinyexr - OpenEXR loader/saver (BSD 3-Clause)
# =========================================================================
set(TINYEXR_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Utils/tinyexr")
if(NOT TARGET tinyexr AND EXISTS "${TINYEXR_DIR}/tinyexr.h")
    add_library(tinyexr STATIC "${TINYEXR_DIR}/tinyexr_impl.cpp")
    target_include_directories(tinyexr PUBLIC
        $<BUILD_INTERFACE:${TINYEXR_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/tinyexr>
    )
    if(TARGET miniz)
        target_link_libraries(tinyexr PRIVATE miniz)
    else()
        message(FATAL_ERROR "tinyexr requires the vendored miniz target")
    endif()
    if(MSVC)
        target_compile_options(tinyexr PRIVATE /W0)
        set_property(TARGET tinyexr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(tinyexr PRIVATE -w)
    endif()
    message(STATUS "Found tinyexr (OpenEXR loader)")
endif()

# Header-only third-party libraries (cross-platform)
set(THIRDPARTY_INCLUDE_DIRS "")

# EnTT (ECS) - prefer the real library, fall back to minimal stub
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include/entt/entt.hpp")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
    message(STATUS "Found EnTT")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt_stub/entt/entt.hpp")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt_stub")
    message(WARNING "EnTT: using minimal stub fallback — run 'git submodule update --init --recursive' for full library")
endif()

# Jolt Physics
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/JoltPhysics/Jolt/Jolt.h")
    set(JOLT_PHYSICS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/JoltPhysics")
    set(JOLT_FOUND TRUE)
    message(STATUS "Found Jolt Physics")
endif()

# ImGui
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui")
    message(STATUS "Found Dear ImGui")
endif()

# AngelScript core plus the add-ons used by AngelScriptEngine. Header-only
# detection is not enough: it produced a runtime stub while making the SDK look
# available in configure output.
set(SPARK_ANGELSCRIPT_SDK_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Scripting/angelscript-mirror/sdk")
set(SPARK_ANGELSCRIPT_CORE_DIR "${SPARK_ANGELSCRIPT_SDK_DIR}/angelscript")
set(SPARK_ANGELSCRIPT_ADDON_DIR "${SPARK_ANGELSCRIPT_SDK_DIR}/add_on")
set(SPARK_ANGELSCRIPT_PATCH_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Scripting/patches")
set(SPARK_ANGELSCRIPT_PATCH_FILE "${SPARK_ANGELSCRIPT_PATCH_DIR}/angelscript-packed-bytecode.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SPARK_ANGELSCRIPT_PATCH_FILE}"
    "${SPARK_ANGELSCRIPT_CORE_DIR}/include/angelscript.h"
    "${SPARK_ANGELSCRIPT_CORE_DIR}/source/as_restore.cpp"
    "${SPARK_ANGELSCRIPT_CORE_DIR}/source/as_scriptengine.cpp")
set(SPARK_ANGELSCRIPT_AVAILABLE FALSE)
set(_SPARK_ANGELSCRIPT_REQUIRED_FILES
    "${SPARK_ANGELSCRIPT_CORE_DIR}/include/angelscript.h"
    "${SPARK_ANGELSCRIPT_CORE_DIR}/source/as_scriptengine.cpp"
    "${SPARK_ANGELSCRIPT_CORE_DIR}/projects/cmake/CMakeLists.txt"
    "${SPARK_ANGELSCRIPT_PATCH_DIR}/angelscript.h"
    "${SPARK_ANGELSCRIPT_PATCH_DIR}/AngelScriptPackedBytecode.h"
    "${SPARK_ANGELSCRIPT_PATCH_FILE}"
    "${SPARK_ANGELSCRIPT_ADDON_DIR}/scriptarray/scriptarray.cpp"
    "${SPARK_ANGELSCRIPT_ADDON_DIR}/scriptbuilder/scriptbuilder.cpp"
    "${SPARK_ANGELSCRIPT_ADDON_DIR}/scriptstdstring/scriptstdstring.cpp"
)
set(_SPARK_ANGELSCRIPT_SDK_COMPLETE TRUE)
foreach(_spark_angelscript_file IN LISTS _SPARK_ANGELSCRIPT_REQUIRED_FILES)
    if(NOT EXISTS "${_spark_angelscript_file}")
        set(_SPARK_ANGELSCRIPT_SDK_COMPLETE FALSE)
    endif()
endforeach()

if(ENABLE_ANGELSCRIPT AND _SPARK_ANGELSCRIPT_SDK_COMPLETE)
    set(AS_DISABLE_INSTALL ON CACHE BOOL "Disable AngelScript's standalone install rules" FORCE)

    # Keep the pinned submodule byte-for-byte immutable. Apply the bounded,
    # reviewable packed-bytecode alignment fix to a fresh build-tree copy so
    # every core translation unit and relative include sees identical sources.
    set(SPARK_ANGELSCRIPT_PATCHED_CORE_DIR "${CMAKE_BINARY_DIR}/ThirdParty/AngelScriptPatched")
    file(REMOVE_RECURSE "${SPARK_ANGELSCRIPT_PATCHED_CORE_DIR}")
    file(COPY "${SPARK_ANGELSCRIPT_CORE_DIR}/" DESTINATION "${SPARK_ANGELSCRIPT_PATCHED_CORE_DIR}")
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                "${GIT_EXECUTABLE}" apply --check --whitespace=error-all "${SPARK_ANGELSCRIPT_PATCH_FILE}"
        WORKING_DIRECTORY "${SPARK_ANGELSCRIPT_PATCHED_CORE_DIR}"
        RESULT_VARIABLE _spark_angelscript_patch_check
        ERROR_VARIABLE _spark_angelscript_patch_check_error)
    if(NOT _spark_angelscript_patch_check EQUAL 0)
        message(FATAL_ERROR
            "AngelScript packed-bytecode patch no longer applies to the pinned source: "
            "${_spark_angelscript_patch_check_error}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                "${GIT_EXECUTABLE}" apply --whitespace=error-all "${SPARK_ANGELSCRIPT_PATCH_FILE}"
        WORKING_DIRECTORY "${SPARK_ANGELSCRIPT_PATCHED_CORE_DIR}"
        RESULT_VARIABLE _spark_angelscript_patch_result
        ERROR_VARIABLE _spark_angelscript_patch_error)
    if(NOT _spark_angelscript_patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply AngelScript packed-bytecode patch: ${_spark_angelscript_patch_error}")
    endif()

    # AngelScript's MSVC x64 backend enables MASM. Directory-wide C/C++ options
    # such as /permissive-, /GL, and /favor are otherwise inherited by ml64 and
    # make its assembly step fail. Configure the dependency in a clean option
    # scope; its own CMake file publishes the language-appropriate flags it needs.
    get_property(_spark_parent_compile_options DIRECTORY PROPERTY COMPILE_OPTIONS)
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
    add_subdirectory(
        "${SPARK_ANGELSCRIPT_PATCHED_CORE_DIR}/projects/cmake"
        "${CMAKE_BINARY_DIR}/ThirdParty/AngelScript"
        EXCLUDE_FROM_ALL
    )
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "${_spark_parent_compile_options}")
    unset(_spark_parent_compile_options)

    # The upstream project defines ANGELSCRIPT_EXPORT even for its default
    # static library. That turns every AngelScript API into dllexport when the
    # archive is consumed by a game DLL; Windows then exposes those functions
    # through the module import library and the host sees duplicate definitions.
    # Static consumers must use the header's no-import/no-export contract.
    if(NOT BUILD_SHARED_LIBS)
        get_target_property(_spark_angelscript_definitions angelscript COMPILE_DEFINITIONS)
        if(_spark_angelscript_definitions)
            list(REMOVE_ITEM _spark_angelscript_definitions ANGELSCRIPT_EXPORT -DANGELSCRIPT_EXPORT)
            set_property(TARGET angelscript PROPERTY COMPILE_DEFINITIONS "${_spark_angelscript_definitions}")
        endif()
        unset(_spark_angelscript_definitions)
    endif()
    target_sources(angelscript PRIVATE
        "${SPARK_ANGELSCRIPT_ADDON_DIR}/scriptarray/scriptarray.cpp"
        "${SPARK_ANGELSCRIPT_ADDON_DIR}/scriptbuilder/scriptbuilder.cpp"
        "${SPARK_ANGELSCRIPT_ADDON_DIR}/scriptstdstring/scriptstdstring.cpp"
    )
    target_include_directories(angelscript PUBLIC
        $<BUILD_INTERFACE:${SPARK_ANGELSCRIPT_ADDON_DIR}>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty>
    )
    target_include_directories(angelscript BEFORE PUBLIC
        $<BUILD_INTERFACE:${SPARK_ANGELSCRIPT_PATCH_DIR}>
    )
    target_compile_definitions(angelscript PUBLIC
        $<BUILD_INTERFACE:SPARK_ANGELSCRIPT_BUILD_TREE=1>
    )
    set_target_properties(angelscript PROPERTIES FOLDER "ThirdParty")
    if(SPARK_SUPPRESS_THIRDPARTY_WARNINGS)
        if(MSVC)
            target_compile_options(angelscript PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/w>)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM")
            target_compile_options(angelscript PRIVATE -w)
        endif()
    endif()

    list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ANGELSCRIPT_ADDON_DIR}")
    set(SPARK_ANGELSCRIPT_AVAILABLE TRUE)
    message(STATUS "AngelScript: enabled (core + scriptarray + scriptbuilder + scriptstdstring)")
elseif(ENABLE_ANGELSCRIPT)
    message(WARNING
        "AngelScript: requested but the vendored SDK is incomplete; scripting will use the explicit unsupported "
        "contract and SparkGameVisualScript will reject module load")
else()
    message(STATUS
        "AngelScript: disabled by ENABLE_ANGELSCRIPT=OFF; SparkGameVisualScript will reject module load")
endif()

# ---------------------------------------------------------------------
# 7.5) Graphics API backend detection (Vulkan, OpenGL)
# ---------------------------------------------------------------------

# --- Vulkan ---
if(ENABLE_VULKAN)
    # On macOS, Vulkan only exists via MoltenVK. Probe Homebrew's install path
    # before find_package so CMake resolves to the MoltenVK loader even when
    # VULKAN_SDK is unset. Users installing through the official Vulkan SDK
    # installer already get VULKAN_SDK set and will hit the normal path below.
    if(APPLE AND NOT DEFINED ENV{VULKAN_SDK})
        foreach(_brew_prefix /opt/homebrew /usr/local)
            if(EXISTS "${_brew_prefix}/opt/molten-vk/lib/libMoltenVK.dylib")
                list(APPEND CMAKE_PREFIX_PATH "${_brew_prefix}/opt/molten-vk")
                message(STATUS "MoltenVK detected at ${_brew_prefix}/opt/molten-vk — adding to CMAKE_PREFIX_PATH")
                break()
            endif()
        endforeach()
    endif()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(SPARK_VULKAN_AVAILABLE TRUE)
        message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
        message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIR}")
    else()
        # Check for Vulkan SDK via environment variable
        if(DEFINED ENV{VULKAN_SDK})
            set(Vulkan_INCLUDE_DIR "$ENV{VULKAN_SDK}/Include")
            if(WIN32)
                set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
            else()
                set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/lib/libvulkan.so")
            endif()
            if(EXISTS "${Vulkan_INCLUDE_DIR}" AND EXISTS "${Vulkan_LIBRARY}")
                set(SPARK_VULKAN_AVAILABLE TRUE)
                message(STATUS "Vulkan SDK found via environment: $ENV{VULKAN_SDK}")
            else()
                set(SPARK_VULKAN_AVAILABLE FALSE)
                message(STATUS "Vulkan SDK path set ($ENV{VULKAN_SDK}) but libraries not found at expected paths")
            endif()
        else()
            set(SPARK_VULKAN_AVAILABLE FALSE)
            message(STATUS "Vulkan not found — backend disabled (see dependency report below for install steps)")
        endif()
    endif()
else()
    set(SPARK_VULKAN_AVAILABLE FALSE)
    message(STATUS "Vulkan backend disabled by option")
endif()

# --- OpenGL ---
if(ENABLE_OPENGL)
    if(NOT OpenGL_FOUND)
        find_package(OpenGL QUIET)
    endif()
    if(OpenGL_FOUND)
        message(STATUS "OpenGL found: ${OPENGL_gl_LIBRARY}")
    else()
        set(SPARK_OPENGL_AVAILABLE FALSE)
        message(STATUS "OpenGL not found - OpenGL backend disabled")
    endif()

    # Look for GLAD loader in ThirdParty
    find_path(GLAD_INCLUDE_DIR
        NAMES glad/glad.h
        PATHS
            "${CMAKE_SOURCE_DIR}/ThirdParty/glad/include"
            "${CMAKE_SOURCE_DIR}/ThirdParty/Graphics/glad/include"
            "${CMAKE_SOURCE_DIR}/ThirdParty/OpenGL/glad/include"
        DOC "GLAD include directory"
    )
    find_path(GLAD_SOURCE_DIR
        NAMES glad.c
        PATHS
            "${CMAKE_SOURCE_DIR}/ThirdParty/glad/src"
            "${CMAKE_SOURCE_DIR}/ThirdParty/Graphics/glad/src"
            "${CMAKE_SOURCE_DIR}/ThirdParty/OpenGL/glad/src"
        DOC "GLAD source directory"
    )
    if(GLAD_INCLUDE_DIR AND GLAD_SOURCE_DIR)
        set(GLAD_FOUND TRUE)
        message(STATUS "GLAD found at: ${GLAD_INCLUDE_DIR}")
    else()
        set(GLAD_FOUND FALSE)
        message(STATUS "GLAD not found - OpenGL loader unavailable (download GLAD for OpenGL support)")
    endif()

    # OpenGL backend requires both OpenGL and GLAD
    if(OpenGL_FOUND AND GLAD_FOUND)
        set(SPARK_OPENGL_AVAILABLE TRUE)
    else()
        set(SPARK_OPENGL_AVAILABLE FALSE)
    endif()

    # EGL for headless/software rendering on Linux (Mesa llvmpipe)
    if(NOT WIN32 AND SPARK_OPENGL_AVAILABLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(EGL QUIET egl)
        endif()
        if(NOT EGL_FOUND)
            find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
            find_library(EGL_LIBRARY NAMES EGL)
            if(EGL_INCLUDE_DIR AND EGL_LIBRARY)
                set(EGL_FOUND TRUE)
            endif()
        endif()
        if(EGL_FOUND)
            set(SPARK_EGL_AVAILABLE TRUE)
            message(STATUS "EGL found — headless software rendering available")
        else()
            set(SPARK_EGL_AVAILABLE FALSE)
            message(STATUS "EGL not found — headless rendering unavailable (install libegl-dev)")
        endif()
    endif()
else()
    set(SPARK_OPENGL_AVAILABLE FALSE)
    message(STATUS "OpenGL backend disabled by option")
endif()

# --- Metal ---
if(ENABLE_METAL AND APPLE)
    # Metal requires Objective-C++ (.mm) sources. The current MetalDevice.h is
    # header-only — flag it so CMake enables the Objective-C++ compiler and
    # surface a clear message until the .mm implementation lands.
    enable_language(OBJCXX)
    set(SPARK_METAL_AVAILABLE TRUE)
    message(STATUS "Metal backend enabled (experimental, requires .mm implementation)")
else()
    set(SPARK_METAL_AVAILABLE FALSE)
    if(ENABLE_METAL AND NOT APPLE)
        message(STATUS "Metal backend requires macOS — disabled on this platform")
    else()
        message(STATUS "Metal backend disabled by option")
    endif()
endif()

# --- OpenAL Soft (audio backend on Linux/macOS) ---
# The OpenALAudioEngine stubs out every AL call unless SPARK_OPENAL_AVAILABLE is
# defined and AL/al.h is reachable. Probe for OpenAL so non-Windows platforms
# actually produce sound. On macOS, Apple ships OpenAL as a system framework
# (deprecated since 10.15 but still functional); openal-soft via Homebrew is
# preferred and searched first.
if(NOT WIN32)
    set(SPARK_OPENAL_AVAILABLE FALSE)
    set(SPARK_OPENAL_LIBRARIES "")
    if(APPLE)
        foreach(_brew_prefix /opt/homebrew /usr/local)
            if(EXISTS "${_brew_prefix}/opt/openal-soft/include/AL/al.h")
                list(APPEND CMAKE_PREFIX_PATH "${_brew_prefix}/opt/openal-soft")
                break()
            endif()
        endforeach()
    endif()
    find_package(OpenAL QUIET)
    if(OPENAL_FOUND)
        set(SPARK_OPENAL_AVAILABLE TRUE)
        if(TARGET OpenAL::OpenAL)
            set(SPARK_OPENAL_LIBRARIES OpenAL::OpenAL)
        else()
            set(SPARK_OPENAL_LIBRARIES ${OPENAL_LIBRARY})
        endif()
        message(STATUS "OpenAL found: ${OPENAL_LIBRARY} (include: ${OPENAL_INCLUDE_DIR})")
    elseif(APPLE)
        # Fall back to the system OpenAL.framework. It still ships in macOS
        # despite the deprecation notice and suffices for our use.
        find_library(OPENAL_FRAMEWORK OpenAL)
        if(OPENAL_FRAMEWORK)
            set(SPARK_OPENAL_AVAILABLE TRUE)
            set(SPARK_OPENAL_LIBRARIES "${OPENAL_FRAMEWORK}")
            message(STATUS "OpenAL: using system OpenAL.framework (install openal-soft via brew for newer builds)")
        endif()
    endif()
    if(NOT SPARK_OPENAL_AVAILABLE)
        message(STATUS "OpenAL not found — audio will be silent on non-Windows (install openal-soft)")
    endif()
endif()

# ---------------------------------------------------------------------
# 8) Build SparkConsole FIRST to ensure it's available for SparkEngine
# ---------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkConsole/CMakeLists.txt")
    message(STATUS "Building SparkConsole...")
    add_subdirectory(SparkConsole)
endif()

# Build SparkShaderCompiler tool
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkShaderCompiler/CMakeLists.txt")
    message(STATUS "Building SparkShaderCompiler tool...")
    add_subdirectory(SparkShaderCompiler)
endif()

# Build SparkCrashReporter (out-of-process crash report handler)
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkCrashReporter/CMakeLists.txt")
    message(STATUS "Building SparkCrashReporter...")
    add_subdirectory(SparkCrashReporter)
endif()

# Build SparkDaemon (long-lived shared-state service: Asset/Shader/Collab/Build)
if(ENABLE_SERVER_PROCESSES AND EXISTS "${CMAKE_SOURCE_DIR}/SparkDaemon/CMakeLists.txt")
    message(STATUS "Building SparkDaemon...")
    add_subdirectory(SparkDaemon)
endif()

# ---------------------------------------------------------------------
# 9) SparkEngine - Static library + Executable
# ---------------------------------------------------------------------
# Architecture:
#   SparkEngineLib  = STATIC LIBRARY (all engine systems)
#   SparkEngine     = EXECUTABLE (entry point + game module loader)
#   SparkGameFPS    = SHARED LIBRARY / DLL (FPS game code, links SparkEngineLib)
#
# The engine exe loads game module DLLs at runtime via GameModuleLoader.
# Both the engine exe and game DLL link SparkEngineLib for shared engine code.
# ---------------------------------------------------------------------

file(GLOB_RECURSE SPARK_ENGINE_SOURCES
    CONFIGURE_DEPENDS
    "SparkEngine/Source/*.cpp"
    "SparkEngine/Source/*.h"
    "SparkEngine/Source/*.hpp"
)

if(APPLE AND SPARK_METAL_AVAILABLE)
    # .mm (Objective-C++) sources only compile when the Metal backend is
    # enabled — OBJCXX is only registered as a language inside the
    # `if(ENABLE_METAL AND APPLE)` block above. Globbing them in without
    # the gate produces a configure error on the macOS OpenGL-only CI run.
    file(GLOB_RECURSE SPARK_ENGINE_OBJCXX_SOURCES
        CONFIGURE_DEPENDS
        "SparkEngine/Source/*.mm"
    )
    list(APPEND SPARK_ENGINE_SOURCES ${SPARK_ENGINE_OBJCXX_SOURCES})
endif()
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*/[Tt]est[^a-z].*|.*/[Tt]ests/.*")
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# Separate entry point files from library sources
# SparkEngine*.cpp contains wWinMain/main + shared globals — they go in the exe only
set(SPARK_ENGINE_ENTRY_POINTS
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngine.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineWindows.cpp"
    # SparkEngineWindows.cpp split parts: exe-only — they define/consume the
    # entry-point globals and (Init/Modules/Win32) the game-mode ImGui overlay,
    # whose enabling compile definition is set on the SparkEngine target only.
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineWindowsHeadless.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineWindowsInit.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineWindowsModules.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineWindowsWin32.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineLinux.cpp"
    # SparkEngineLinux.cpp split parts: executable-only. They consume the
    # process entry-point globals declared by SparkEngineLinuxInternal.h and
    # implement the headless, SDL2, and shared POSIX startup/shutdown paths.
    # Keeping them in SparkEngineLib makes the reusable archive depend on
    # globals that only the SparkEngine executable owns.
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineLinuxHeadless.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineLinuxInit.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineLinuxSDL2.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineLinuxSDL2Events.cpp"
    # Game-mode ImGui overlay: exe-only (owns the ImGui context that is
    # injected into module DLLs; must NOT live in SparkEngineLib which the
    # module DLLs also link).
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/GameImGuiLayer.cpp"
    # NOTE: SparkEngineMacOS.cpp is intentionally NOT an entry point — it
    # provides helpers (Metal view lifecycle, exe-path lookup) that are
    # called from SparkEngineLinux.cpp and from tests. It lives in
    # SparkEngineLib so every consumer (exe + SparkTests) sees the symbols.
)
set(SPARK_ENGINE_LIB_SOURCES ${SPARK_ENGINE_SOURCES})
foreach(_ep ${SPARK_ENGINE_ENTRY_POINTS})
    list(REMOVE_ITEM SPARK_ENGINE_LIB_SOURCES "${_ep}")
endforeach()

# The engine archive is reused by non-SparkEngine hosts (SparkServer, tools,
# tests), so it must never retain an executable entry-point split. Require all
# current and future SparkEngineLinux*/SparkEngineWindows* implementation
# splits to be registered above instead of silently entering the library glob.
file(GLOB _SPARK_PLATFORM_ENTRY_POINT_SPLITS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineLinux*.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngineWindows*.cpp"
)
foreach(_platform_ep ${_SPARK_PLATFORM_ENTRY_POINT_SPLITS})
    if(NOT _platform_ep IN_LIST SPARK_ENGINE_ENTRY_POINTS)
        message(FATAL_ERROR "Platform entry-point split is not registered in SPARK_ENGINE_ENTRY_POINTS: ${_platform_ep}")
    endif()
endforeach()
foreach(_ep ${SPARK_ENGINE_ENTRY_POINTS})
    if(_ep IN_LIST SPARK_ENGINE_LIB_SOURCES)
        message(FATAL_ERROR "Executable entry-point source leaked into SparkEngineLib: ${_ep}")
    endif()
endforeach()
unset(_SPARK_PLATFORM_ENTRY_POINT_SPLITS)

# Exclude source files whose third-party dependencies are missing
if(NOT JOLT_FOUND)
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystem\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystemQueries\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsShapeFactory\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsBodyImpl\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsConsoleOps\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsConstraints\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsMotorControl\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSpatialQueries\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSpatialQueriesCasts\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSpatialQueriesFiltered\\.cpp$")
    message(WARNING "Jolt Physics not found — using PhysicsSystemStub.cpp fallback (no rigid body physics)")
else()
    # When the real PhysicsSystem is built, exclude the stub
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystemStub\\.cpp$")
endif()
if(NOT MINIZ_FOUND)
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashHandler\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashHandlerHelpers\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashReportUploader\\.cpp$")
    message(WARNING "MINIZ not found — using CrashHandlerStub.cpp fallback (no crash dump compression)")
else()
    # When the real CrashHandler is built, exclude the stub
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashHandlerStub\\.cpp$")
endif()

# MinGW's d3d12.h headers are too old for the full D3D12 backend (missing
# ID3D12Device5, debug layer types, mesh shaders, VRS). Exclude D3D12 and DXR
# sources on MinGW — D3D11 is the primary backend and works perfectly under Wine + DXVK.
if(MINGW AND NOT MSVC)
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/RHI/D3D12/.*")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/RHI/DXRSupport\\.cpp$")
    add_compile_definitions(SPARK_NO_D3D12)
    message(STATUS "MinGW: D3D12/DXR backend excluded (headers too old). D3D11 is primary.")

    # Copy libwinpthread-1.dll to the output bin/ directory so Wine can find it.
    # MinGW's posix threading model requires this DLL at runtime.
    find_file(MINGW_WINPTHREAD_DLL "libwinpthread-1.dll"
              PATHS /usr/x86_64-w64-mingw32/lib /usr/lib/gcc/x86_64-w64-mingw32
              NO_DEFAULT_PATH)
    if(MINGW_WINPTHREAD_DLL)
        message(STATUS "MinGW: Will copy ${MINGW_WINPTHREAD_DLL} to output bin/")
    endif()
endif()

# 9a) SparkEngineLib - Static library with all engine systems
add_library(SparkEngineLib STATIC ${SPARK_ENGINE_LIB_SOURCES})
set_target_properties(SparkEngineLib PROPERTIES OUTPUT_NAME "SparkEngineLib")

# 9a.5) SparkEngineInterface — header-only view of SparkEngineLib
#
# On Linux, game module .so files must NOT link SparkEngineLib's object
# code, otherwise each .so gets its own copy of every singleton and the
# class layout diverges when conditional members (#ifdef ENABLE_NETWORKING)
# change member offsets.  Instead, game modules link this INTERFACE
# target, which provides the same include directories and compile
# definitions but zero object code.  Undefined symbols are resolved at
# dlopen() time from the host executable (which has ENABLE_EXPORTS ON).
#
# macOS: ld64 rejects undefined symbols in .dylib by default. Pass
# `-undefined dynamic_lookup` so resolution is deferred to dlopen, the
# same way Linux's `ld` already permits unresolved symbols in shared
# libraries. Without this, every game module fails to link with
# "Undefined symbols for architecture arm64" against SparkEngineLib
# symbols (SimpleConsole, Logger, AngelScriptEngine, etc.).
#
# IMPORTANT: this target must be defined BEFORE game modules are added
# via add_subdirectory() in section 9.5, because each module's
# CMakeLists checks `if(TARGET SparkEngineInterface)` at configure time.
# The include-dirs / compile-defs generator expressions are evaluated at
# generate time, so they still pick up properties added later (e.g. in
# section 11.6 / 12 / 13).
if(NOT WIN32)
    add_library(SparkEngineInterface INTERFACE)
    target_include_directories(SparkEngineInterface INTERFACE
        $<BUILD_INTERFACE:$<TARGET_PROPERTY:SparkEngineLib,INTERFACE_INCLUDE_DIRECTORIES>>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/SparkEngine>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty>)
    target_compile_definitions(SparkEngineInterface INTERFACE
        $<BUILD_INTERFACE:$<TARGET_PROPERTY:SparkEngineLib,INTERFACE_COMPILE_DEFINITIONS>>)
    target_compile_features(SparkEngineInterface INTERFACE cxx_std_23)
    if(APPLE)
        target_link_options(SparkEngineInterface INTERFACE
            "LINKER:-undefined,dynamic_lookup")
    endif()
endif()

# 9b) SparkEngine - Executable (entry point + module loader)
if(WIN32)
    add_executable(SparkEngine WIN32 ${SPARK_ENGINE_ENTRY_POINTS})
else()
    add_executable(SparkEngine ${SPARK_ENGINE_ENTRY_POINTS})
endif()
if(WIN32)
    target_link_libraries(SparkEngine PRIVATE SparkEngineLib)
else()
    # POSIX game modules resolve engine calls from the host at dlopen() time.
    # Retain archive members that the executable itself does not reference so
    # ENABLE_EXPORTS/-export_dynamic below can expose the complete SDK surface.
    target_link_libraries(SparkEngine PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,SparkEngineLib>")
endif()
# MinGW: -municode enables wWinMain as the entry point (Unicode command line)
if(MINGW)
    target_link_options(SparkEngine PRIVATE -municode)
endif()

# Game-mode ImGui overlay (module HUDs without the editor). Reuses the shared
# `imgui` static library target (core + win32/dx11 backends on Windows).
if(WIN32)
    include("${CMAKE_SOURCE_DIR}/cmake/BuildImGui.cmake")
    if(TARGET imgui)
        target_link_libraries(SparkEngine PRIVATE imgui)
        target_compile_definitions(SparkEngine PRIVATE SPARK_HAS_IMGUI=1)
        message(STATUS "SparkEngine: game-mode ImGui overlay ENABLED")
    endif()
endif()

# On Linux/macOS, export the executable's symbols so that game module .so files
# loaded via dlopen() can resolve SparkEngineLib symbols from the host process.
# Without this, each .so would need its own copy of SparkEngineLib, creating
# duplicate singletons (NetworkManager, EngineContext, etc.) and ABI mismatches.
if(NOT WIN32)
    set_target_properties(SparkEngine PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        # Darwin's ENABLE_EXPORTS support primarily enables CMake's
        # -bundle_loader relationship when a module target links an executable.
        # Spark game dylibs are host-agnostic and use dynamic_lookup instead,
        # so preserve the executable's complete global symbol surface explicitly.
        target_link_options(SparkEngine PRIVATE "LINKER:-export_dynamic")
    endif()
endif()

# Link third-party libraries to the static lib (both exe and game DLL get them)
if(MINIZ_FOUND)
    # Public because installed inline headers (ScreenCapture and tinyexr-backed
    # loaders) compile miniz call sites in downstream translation units.
    target_link_libraries(SparkEngineLib PUBLIC miniz)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_MINIZ_AVAILABLE=1)
    target_compile_definitions(SparkEngine PRIVATE SPARK_MINIZ_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with MINIZ")
endif()

if(SPARK_ANGELSCRIPT_AVAILABLE)
    # The public feature definition keeps AngelScriptEngine's class layout and
    # behavior identical in the host, game modules, tests, and SDK consumers.
    # The static library dependency itself is link-only for consumers.
    target_link_libraries(SparkEngineLib PRIVATE angelscript)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_ANGELSCRIPT_SUPPORT=1)
    message(STATUS "Linking SparkEngineLib with AngelScript runtime and required add-ons")
endif()

if(ENABLE_NETWORKING)
    # Detect curl before the linkage block so clean and cached configure runs
    # produce the same SparkEngineLib link interface.
    find_package(CURL QUIET)
endif()

# zstd — Zstandard compression (faster than miniz, used alongside it)
if(TARGET zstd)
    target_link_libraries(SparkEngineLib PRIVATE zstd)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_ZSTD_AVAILABLE=1)
    target_compile_definitions(SparkEngine PRIVATE SPARK_ZSTD_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with zstd")
endif()

# VMA — Vulkan Memory Allocator (header-only, Vulkan only)
if(TARGET VulkanMemoryAllocator AND SPARK_VULKAN_AVAILABLE)
    target_link_libraries(SparkEngineLib PRIVATE $<BUILD_INTERFACE:VulkanMemoryAllocator>)
    message(STATUS "Linking SparkEngineLib with VulkanMemoryAllocator")
endif()

if(CURL_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE CURL::libcurl)
    message(STATUS "Linking SparkEngineLib with libcurl")
endif()

if(TINYOBJ_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE tinyobjloader)
    message(STATUS "Linking SparkEngineLib with TinyObjLoader")
endif()

# Link stb_image (cross-platform texture loading)
# PUBLIC because TextureSystem.h consumers may transitively need it
if(TARGET stb_image)
    target_link_libraries(SparkEngineLib PUBLIC stb_image)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HAS_STB_IMAGE=1)
    message(STATUS "Linking SparkEngineLib with stb_image")
endif()

# Link cgltf (glTF 2.0 model loading)
# PUBLIC because AssetPipeline.h consumers may transitively need it
if(TARGET cgltf)
    target_link_libraries(SparkEngineLib PUBLIC cgltf)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HAS_CGLTF=1)
    message(STATUS "Linking SparkEngineLib with cgltf")
endif()

# Link miniaudio (cross-platform audio backend)
# PUBLIC because PlatformAudioStubs.h includes miniaudio.h transitively
if(TARGET miniaudio)
    target_link_libraries(SparkEngineLib PUBLIC miniaudio)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HAS_MINIAUDIO=1)
    message(STATUS "Linking SparkEngineLib with miniaudio")
endif()

# Link nlohmann/json (JSON parsing library)
# PUBLIC because JsonUtils.h includes nlohmann_json.h when available
if(TARGET nlohmann_json)
    target_link_libraries(SparkEngineLib PUBLIC nlohmann_json)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HAS_NLOHMANN_JSON=1)
    message(STATUS "Linking SparkEngineLib with nlohmann/json")
endif()

# Link tinyexr (OpenEXR image loading)
# PUBLIC because TextureSystem consumers may transitively need it
if(TARGET tinyexr)
    target_link_libraries(SparkEngineLib PUBLIC tinyexr)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HAS_TINYEXR=1)
    message(STATUS "Linking SparkEngineLib with tinyexr")
endif()

# Link Jolt Physics
if(JOLT_FOUND)
    # Configure Jolt build options
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
    # Gate Jolt's IPO on our top-level ENABLE_LTO. When ENABLE_LTO=OFF the
    # engine is compiled as plain ELF/native objects; enabling IPO only on
    # Jolt leaves libJolt.a full of LLVM bitcode (Clang) or gcc-lto sections
    # (GCC without the linker plugin), which GNU ld then rejects with
    # "libJolt.a: error adding symbols: file format not recognized".
    if(ENABLE_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|MinSizeRel|RelWithDebInfo")
        set(INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)
    else()
        set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    endif()
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    # RTTI must be ON to match the engine — otherwise PhysicsShapeFactory
    # generates typeinfo refs that the Jolt library cannot satisfy.
    set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)

    # Double precision physics for large-world support (MMO, open-world)
    if(SPARK_DOUBLE_PRECISION_PHYSICS)
        set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
        message(STATUS "Jolt Physics: double precision enabled (JPH_DOUBLE_PRECISION)")
    endif()

    # Use dynamic CRT to match the rest of the engine (/MD instead of /MT)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

    # Jolt GPU-accelerated physics (optional)
    # Jolt can offload certain physics workloads (hair simulation, compute tests)
    # to the GPU via Vulkan, DirectX 12, or Metal compute shaders.  Each backend
    # requires a shader compiler at configure time; if the compiler is missing the
    # backend is silently disabled and Jolt falls back to CPU-only physics (which
    # is the normal mode for most projects).
    #
    # Vulkan compute — requires glslc (ships with the Vulkan SDK)
    find_package(Vulkan QUIET COMPONENTS glslc)
    if(TARGET Vulkan::glslc)
        message(STATUS "Jolt GPU compute: Vulkan ENABLED (glslc found)")
    else()
        set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
        message(STATUS "Jolt GPU compute: Vulkan DISABLED (glslc not found)")
        message(STATUS "  What: glslc compiles GLSL/HLSL shaders to SPIR-V for Vulkan compute")
        message(STATUS "  Why:  Enables GPU-accelerated physics (hair sim, parallel broadphase)")
        message(STATUS "  Install:")
        message(STATUS "    Windows/Linux : Install the Vulkan SDK — https://vulkan.lunarg.com/sdk/home")
        message(STATUS "    Ubuntu/Debian : sudo apt install vulkan-tools glslang-tools")
        message(STATUS "    Fedora        : sudo dnf install vulkan-tools glslang")
        message(STATUS "    macOS         : brew install vulkan-sdk")
    endif()

    # DX12 compute — requires dxc (Microsoft DirectX Shader Compiler)
    find_program(_SPARK_DXC dxc QUIET)
    if(_SPARK_DXC)
        message(STATUS "Jolt GPU compute: DX12 ENABLED (dxc found: ${_SPARK_DXC})")
    else()
        set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
        message(STATUS "Jolt GPU compute: DX12 DISABLED (dxc not found)")
        message(STATUS "  What: dxc compiles HLSL shaders to DXIL for DirectX 12 compute")
        message(STATUS "  Why:  Enables GPU-accelerated physics on Windows via DX12")
        message(STATUS "  Install:")
        message(STATUS "    Windows : Included with Windows SDK / Visual Studio (ensure 'Graphics Tools' is installed)")
        message(STATUS "    Linux   : https://github.com/microsoft/DirectXShaderCompiler/releases")
        message(STATUS "    macOS   : brew install microsoft/dxc/directx-shader-compiler")
    endif()

    # Metal compute — requires xcrun (Xcode command-line tools, macOS only)
    find_program(_SPARK_XCRUN xcrun QUIET)
    if(_SPARK_XCRUN)
        message(STATUS "Jolt GPU compute: Metal ENABLED (xcrun found: ${_SPARK_XCRUN})")
    else()
        set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
        if(APPLE)
            message(STATUS "Jolt GPU compute: Metal DISABLED (xcrun not found)")
            message(STATUS "  What: xcrun invokes Apple's Metal shader compiler toolchain")
            message(STATUS "  Why:  Enables GPU-accelerated physics on macOS/iOS via Metal")
            message(STATUS "  Install: xcode-select --install")
        else()
            message(STATUS "Jolt GPU compute: Metal DISABLED (not macOS)")
        endif()
    endif()

    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${JOLT_PHYSICS_SOURCE_DIR}/Build" "${CMAKE_BINARY_DIR}/JoltPhysics")

    # Suppress warnings in third-party Jolt code
    if(SPARK_SUPPRESS_THIRDPARTY_WARNINGS AND TARGET Jolt)
        if(MSVC)
            target_compile_options(Jolt PRIVATE /w)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM")
            target_compile_options(Jolt PRIVATE -w)
        endif()
    endif()

    target_link_libraries(SparkEngineLib PRIVATE Jolt)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_JOLT_PHYSICS_AVAILABLE=1)
    target_compile_definitions(SparkEngine PRIVATE SPARK_JOLT_PHYSICS_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with Jolt Physics")
endif()

# Recast/Detour — NavMesh generation library
# Build the library targets if sources exist
set(RECAST_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/AI/recastnavigation")
if(ENABLE_RECAST AND NOT TARGET Recast AND NOT TARGET Detour AND EXISTS "${RECAST_DIR}/Recast/Source/Recast.cpp")
    file(GLOB RECAST_SOURCES "${RECAST_DIR}/Recast/Source/*.cpp")
    add_library(Recast STATIC ${RECAST_SOURCES})
    target_include_directories(Recast PUBLIC
        $<BUILD_INTERFACE:${RECAST_DIR}/Recast/Include>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/recast/Recast>
    )
    if(MSVC)
        target_compile_options(Recast PRIVATE /W0)
    else()
        target_compile_options(Recast PRIVATE -w)
    endif()

    file(GLOB DETOUR_SOURCES "${RECAST_DIR}/Detour/Source/*.cpp")
    add_library(Detour STATIC ${DETOUR_SOURCES})
    target_include_directories(Detour PUBLIC
        $<BUILD_INTERFACE:${RECAST_DIR}/Detour/Include>
        $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/recast/Detour>
    )
    if(MSVC)
        target_compile_options(Detour PRIVATE /W0)
    else()
        target_compile_options(Detour PRIVATE -w)
    endif()

    set(RECAST_FOUND TRUE)
    message(STATUS "Recast/Detour: configured from ThirdParty/AI/recastnavigation")
else()
    set(RECAST_FOUND FALSE)
    if(ENABLE_RECAST)
        message(STATUS "Recast/Detour: not found (submodule not initialized)")
    endif()
endif()

if(RECAST_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE Recast Detour)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_RECAST_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with Recast/Detour")
endif()

# SparkConsole dependency
if(TARGET SparkConsole)
    add_dependencies(SparkEngine SparkConsole)
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkConsole>
            $<TARGET_FILE_DIR:SparkEngine>
        COMMENT "Copying SparkConsole to engine output directory"
        VERBATIM
    )
    message(STATUS "SparkEngine will be built after SparkConsole")
endif()

# ---------------------------------------------------------------------
# 9.4) Standalone runtime and production-pipeline processes
# ---------------------------------------------------------------------
if(ENABLE_SERVER_PROCESSES)
    if(EXISTS "${CMAKE_SOURCE_DIR}/SparkGateway/CMakeLists.txt")
        add_subdirectory(SparkGateway)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/SparkServer/CMakeLists.txt")
        add_subdirectory(SparkServer)
    endif()
endif()

if(ENABLE_ASSET_PIPELINE_TOOLS)
    if(EXISTS "${CMAKE_SOURCE_DIR}/SparkAssetPipelineCore/CMakeLists.txt")
        add_subdirectory(SparkAssetPipelineCore)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/SparkCooker/CMakeLists.txt")
        add_subdirectory(SparkCooker)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/SparkWorker/CMakeLists.txt")
        add_subdirectory(SparkWorker)
    endif()
endif()

if(ENABLE_AUTOMATION_HOST AND EXISTS "${CMAKE_SOURCE_DIR}/SparkAutomation/CMakeLists.txt")
    add_subdirectory(SparkAutomation)
endif()

# ---------------------------------------------------------------------
# 9.4) Runtime shader staging (one copier for every consumer)
#
# SparkEngine and every in-tree game module share bin/<config>/Shaders.
# Each of them used to copy the same HLSL files there from its own
# POST_BUILD step, and parallel MSBuild module builds raced on the
# destination ("Error copying file (if different)"). A single custom
# target stages the files; the engine and the modules depend on it.
# ---------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SHADER_FILES "${CMAKE_SOURCE_DIR}/SparkEngine/Shaders/HLSL/*.hlsl")
set(_SPARK_SHADER_STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Shaders")
foreach(SHADER_FILE ${ENGINE_SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    list(APPEND _SPARK_SHADER_STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHADER_FILE}"
            "$<TARGET_FILE_DIR:SparkEngine>/Shaders/${SHADER_NAME}")
endforeach()

# Also stage the top-level Shaders/HLSL/ tree (DXR ray-tracing shaders,
# foliage impostor bake shader, compute shaders, mesh shaders). These
# live under the repo root rather than SparkEngine/Shaders/HLSL/ for
# historical reasons; the runtime paths in DXRSupport.cpp and
# FoliageImpostorBaker.cpp expect them at "Shaders/HLSL/...".
if(EXISTS "${CMAKE_SOURCE_DIR}/Shaders/HLSL")
    list(APPEND _SPARK_SHADER_STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Shaders/HLSL"
            "$<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL")
endif()

add_custom_target(SparkRuntimeShaders
    ${_SPARK_SHADER_STAGE_COMMANDS}
    COMMENT "Staging HLSL shaders into the runtime output directory"
    VERBATIM
)
add_dependencies(SparkEngine SparkRuntimeShaders)
unset(_SPARK_SHADER_STAGE_COMMANDS)

# ---------------------------------------------------------------------
# 9.5) Build game modules from GameModules/ directory (auto-discovery)
#
# Drop a game module folder into GameModules/ with a CMakeLists.txt and
# it will be built automatically.  Remove the folder to exclude it.
# Set BUILD_GAME_MODULES=OFF to skip all game modules entirely.
# ---------------------------------------------------------------------
if(NOT DEFINED SPARK_ENGINE_INCLUDE_DIR OR SPARK_ENGINE_INCLUDE_DIR STREQUAL "")
    set(SPARK_ENGINE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/SparkPlugin.cmake")

if(BUILD_GAME_MODULES)
    file(GLOB _SPARK_GAME_MODULE_DIRS "${CMAKE_SOURCE_DIR}/GameModules/*/CMakeLists.txt")
    list(LENGTH _SPARK_GAME_MODULE_DIRS _SPARK_GAME_MODULE_COUNT)

    if(_SPARK_GAME_MODULE_COUNT EQUAL 0)
        message(STATUS "Game modules: none found in GameModules/")
    endif()

    set(_SPARK_GAME_MODULE_NAMES "")
    foreach(_MOD_CMAKE IN LISTS _SPARK_GAME_MODULE_DIRS)
        get_filename_component(_MOD_DIR "${_MOD_CMAKE}" DIRECTORY)
        get_filename_component(_MOD_NAME "${_MOD_DIR}" NAME)

        message(STATUS "Adding game module: ${_MOD_NAME}")
        add_subdirectory("${_MOD_DIR}" "${CMAKE_BINARY_DIR}/GameModules/${_MOD_NAME}")

        if(TARGET ${_MOD_NAME})
            # On Windows, link the import library so the engine can resolve
            # DLL exports at load time.  On Linux/macOS, game modules are
            # loaded at runtime via dlopen (ModuleManager), so we must NOT
            # create an ELF NEEDED dependency — that would force the shared
            # library to load at startup and cause duplicate-singleton issues
            # when static SparkEngineLib symbols appear in both the exe and SO.
            if(WIN32)
                target_link_libraries(SparkEngine PRIVATE ${_MOD_NAME})
                target_link_libraries(SparkEngine PRIVATE SparkEngineLib)
            endif()
            add_custom_command(TARGET SparkEngine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_MOD_NAME}>
                    $<TARGET_FILE_DIR:SparkEngine>
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_MOD_NAME}>.sparkabi"
                    $<TARGET_FILE_DIR:SparkEngine>
                COMMENT "Copying ${_MOD_NAME} DLL to engine output directory"
                VERBATIM
            )
            list(APPEND _SPARK_GAME_MODULE_NAMES "${_MOD_NAME}")
        endif()
    endforeach()
else()
    message(STATUS "Game modules: SKIPPED (BUILD_GAME_MODULES=OFF)")
endif()
set(SPARK_PACKAGE_GAME_MODULES "${_SPARK_GAME_MODULE_NAMES}")

# ---------------------------------------------------------------------
# 9.6) Build SparkEditor if enabled and exists
# ---------------------------------------------------------------------
set(SPARK_EDITOR_DEPS_OK TRUE)
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui/imgui.h")
    set(SPARK_EDITOR_DEPS_OK FALSE)
    set(SPARK_EDITOR_SKIP_REASON "Dear ImGui not found (initialize submodule)")
elseif(NOT WIN32)
    # Linux editor requires SDL2 and OpenGL — check before adding subdirectory
    # SDL2 may come from the ThirdParty submodule (preferred) or system install
    if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2)
        find_package(SDL2 QUIET)
    else()
        set(SDL2_FOUND TRUE)
    endif()
    find_package(OpenGL QUIET)
    if(NOT SDL2_FOUND AND NOT TARGET SDL2 AND NOT TARGET SDL2::SDL2)
        set(SPARK_EDITOR_DEPS_OK FALSE)
        set(SPARK_EDITOR_SKIP_REASON "SDL2 not found (run: git submodule update --init --recursive)")
    elseif(NOT OpenGL_FOUND)
        set(SPARK_EDITOR_DEPS_OK FALSE)
        set(SPARK_EDITOR_SKIP_REASON "OpenGL not found (install libgl-dev)")
    endif()
endif()

if(ENABLE_EDITOR AND EXISTS "${CMAKE_SOURCE_DIR}/SparkEditor/CMakeLists.txt" AND SPARK_EDITOR_DEPS_OK)
    message(STATUS "Adding SparkEditor to build...")
    add_subdirectory(SparkEditor)
elseif(ENABLE_EDITOR AND NOT SPARK_EDITOR_DEPS_OK)
    message(STATUS "SparkEditor skipped — ${SPARK_EDITOR_SKIP_REASON}")

    if(TARGET SparkEngine AND TARGET SparkEditor)
        add_dependencies(SparkEditor SparkEngine)
        message(STATUS "SparkEditor will be built after SparkEngine")
    endif()
endif()

if(ENABLE_LAUNCHER AND EXISTS "${CMAKE_SOURCE_DIR}/SparkLauncher/CMakeLists.txt")
    message(STATUS "Adding SparkLauncher to build...")
    add_subdirectory(SparkLauncher)
endif()

if(ENABLE_SPARKBUILD AND EXISTS "${CMAKE_SOURCE_DIR}/SparkBuild/CMakeLists.txt")
    message(STATUS "Adding SparkBuild to build...")
    add_subdirectory(SparkBuild)
endif()

# SparkInstaller depends on SparkBuildCore (defined by SparkBuild/CMakeLists.txt), so
# it must be added after SparkBuild.
if(ENABLE_INSTALLER AND EXISTS "${CMAKE_SOURCE_DIR}/SparkInstaller/CMakeLists.txt")
    if(TARGET SparkBuildCore)
        message(STATUS "Adding SparkInstaller to build...")
        add_subdirectory(SparkInstaller)
    else()
        message(STATUS "ENABLE_INSTALLER=ON but SparkBuildCore is not available — skipping SparkInstaller")
    endif()
endif()

# ---------------------------------------------------------------------
# 10) Asset Management - Asset directories for the engine output
#
# HLSL shaders are staged by the SparkRuntimeShaders target (section 9.4).
# ---------------------------------------------------------------------
add_custom_command(TARGET SparkEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Assets"
    COMMENT "Creating engine asset directory structure"
)

# DXR ray-tracing shaders need DXC (lib_6_3 profile) to compile to DXIL
# blobs. d3dcompiler.dll caps out at SM 5.1 and cannot produce ray
# tracing libraries. We invoke dxc.exe at build time when:
#   - Building on Windows MSVC (NOT MinGW — MinGW's d3d12.h is too old
#     to host DXR anyway)
#   - ENABLE_DXR is on (which gates the C++ side too)
#   - find_program located dxc on the host
#
# When dxc is missing the build still succeeds — the runtime
# DXRManager::BuildRTPSOs() handles missing .cso files by logging a
# warning and skipping that PSO, so trace methods become no-ops.
if(ENABLE_DXR AND WIN32 AND NOT MINGW)
    find_program(DXC_EXECUTABLE
        NAMES dxc dxc.exe
        HINTS
            "$ENV{WindowsSdkVerBinPath}x64"
            "$ENV{WindowsSdkDir}bin/x64"
            "$ENV{VULKAN_SDK}/Bin"
        DOC "DirectX Shader Compiler (dxc.exe) — required for DXR HLSL → DXIL"
    )

    if(DXC_EXECUTABLE)
        message(STATUS "SparkEngine: Found dxc at ${DXC_EXECUTABLE} — will compile DXR shaders to DXIL")
        set(DXR_SHADER_DIR "${CMAKE_SOURCE_DIR}/Shaders/HLSL/RayTracing")
        set(DXR_OUTPUT_DIR "$<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL/RayTracing")

        # Each DXR HLSL file is compiled as a `lib_6_3` DXIL library.
        # Library compilation discovers all functions tagged with
        # [shader("...")] attributes, so no -E entry flag is needed.
        # The output .cso is written next to the runtime .hlsl file.
        set(DXR_HLSL_FILES
            DXRReflections.hlsl
            DXRShadows.hlsl
            DXRAO.hlsl
            DXRGI.hlsl
        )

        set(DXR_CSO_OUTPUTS "")
        foreach(SHADER ${DXR_HLSL_FILES})
            get_filename_component(SHADER_BASE ${SHADER} NAME_WE)
            set(SHADER_INPUT "${DXR_SHADER_DIR}/${SHADER}")
            set(SHADER_OUTPUT "${DXR_OUTPUT_DIR}/${SHADER_BASE}.cso")
            add_custom_command(TARGET SparkEngine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DXR_OUTPUT_DIR}"
                COMMAND ${DXC_EXECUTABLE}
                    -T lib_6_3
                    -Fo "${SHADER_OUTPUT}"
                    "${SHADER_INPUT}"
                COMMENT "DXC: ${SHADER} -> ${SHADER_BASE}.cso (lib_6_3)"
                VERBATIM
            )
            list(APPEND DXR_CSO_OUTPUTS "${SHADER_OUTPUT}")
        endforeach()
    else()
        message(STATUS "SparkEngine: dxc not found — DXR HLSL files will not be compiled to DXIL")
        message(STATUS "  Install the latest Windows 10/11 SDK or set WindowsSdkVerBinPath in your environment.")
        message(STATUS "  Trace methods will become no-ops at runtime (logged as warnings, no crashes).")
    endif()
endif()

# Phase G: Offline FXC validation for the foliage shader family.
#
# The foliage VS/PS pair (Phase E) and the impostor bake VS/PS pair
# (Phase C) are still compiled at runtime via D3DCompileFromFile. That
# path is correct and shipped, but it defers shader errors to the first
# time a foliage scene is loaded — which in CI never happens.
#
# This step runs fxc.exe at build time as a belt-and-braces validation:
# the runtime code is unchanged, but if any of the four shaders fail to
# compile the build fails with the exact error from the real Windows
# shader compiler. The generated .cso files are dropped next to the
# .hlsl copies under $<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL/ so a
# future session can switch the runtime to offline-load with a one-line
# change to CompileFoliageShader.
#
# Gated the same way as the DXC step (Windows MSVC only, non-fatal when
# fxc is missing) so Linux and MinGW builds are unaffected.
if(WIN32 AND NOT MINGW)
    find_program(FXC_EXECUTABLE
        NAMES fxc fxc.exe
        HINTS
            "$ENV{WindowsSdkVerBinPath}x64"
            "$ENV{WindowsSdkDir}bin/x64"
        DOC "Legacy HLSL compiler (fxc.exe) — validates SM 5.0 shaders at build time"
    )

    if(FXC_EXECUTABLE)
        message(STATUS "SparkEngine: Found fxc at ${FXC_EXECUTABLE} — will validate foliage shaders offline")
        set(FOLIAGE_SHADER_DIR "${CMAKE_SOURCE_DIR}/Shaders/HLSL")
        set(FOLIAGE_OUTPUT_DIR "$<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL")

        # Pairs of (file, entry, profile). FoliageVS/PS use `main`; the
        # impostor bake pair uses VSMain/PSMain per FoliageImpostorBaker
        # CompileBakeShaders.
        set(FOLIAGE_FXC_TARGETS
            "FoliageVS.hlsl|main|vs_5_0"
            "FoliagePS.hlsl|main|ps_5_0"
            "FoliageImpostorBake.hlsl|VSMain|vs_5_0"
            "FoliageImpostorBake.hlsl|PSMain|ps_5_0"
        )

        foreach(TARGET_SPEC ${FOLIAGE_FXC_TARGETS})
            string(REPLACE "|" ";" TARGET_PARTS ${TARGET_SPEC})
            list(GET TARGET_PARTS 0 SHADER_FILE)
            list(GET TARGET_PARTS 1 ENTRY_POINT)
            list(GET TARGET_PARTS 2 PROFILE)

            get_filename_component(SHADER_BASE ${SHADER_FILE} NAME_WE)
            set(SHADER_INPUT "${FOLIAGE_SHADER_DIR}/${SHADER_FILE}")
            set(SHADER_OUTPUT "${FOLIAGE_OUTPUT_DIR}/${SHADER_BASE}_${ENTRY_POINT}.cso")

            add_custom_command(TARGET SparkEngine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${FOLIAGE_OUTPUT_DIR}"
                COMMAND ${FXC_EXECUTABLE}
                    /nologo
                    /T ${PROFILE}
                    /E ${ENTRY_POINT}
                    /Fo "${SHADER_OUTPUT}"
                    "${SHADER_INPUT}"
                COMMENT "FXC: ${SHADER_FILE} [${ENTRY_POINT}/${PROFILE}] -> ${SHADER_BASE}_${ENTRY_POINT}.cso"
                VERBATIM
            )
        endforeach()
    else()
        message(STATUS "SparkEngine: fxc not found — foliage shaders will only be validated at runtime")
        message(STATUS "  Install the Windows 10/11 SDK or set WindowsSdkVerBinPath in your environment")
        message(STATUS "  if you want build-time validation of FoliageVS/PS + FoliageImpostorBake.")
    endif()
endif()

# Copy all asset subdirectories (Models, Textures, Audio, Materials, Scenes, Scripts, MMO,
# MMOFPS). MMOFPS holds Assets/MMOFPS/Data/*.json (weapons/classes/regions/factions/vehicles/
# suits/continents/decor/presentation) — TFDataTables::Initialize() loads these via a
# hardcoded cwd-relative "Assets/MMOFPS/Data/" path (Data/TFDataTables.cpp), so without this
# entry the TERRAFRONT module's data tables (and therefore the whole module, and every tf_*
# console command) silently fail to boot whenever SparkEngine.exe is run from the build output
# directory instead of the repo root. Found via the W13 integration acceptance pass
# (2026-07-12): chaos_accept.cfg's tf_dedicated/tf_chaos/tf_validate all came back "Unknown
# command" because TFDataTables::Initialize() returned false before the module could register
# them. SparkTests.exe never hit this because its TFData_* tests walk up to the repo root
# instead of trusting cwd.
foreach(ASSET_DIR Models Textures Audio Materials Scenes Scripts MMO MMOFPS Engine)
    if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/${ASSET_DIR}")
        add_custom_command(TARGET SparkEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/Assets/${ASSET_DIR}"
                "$<TARGET_FILE_DIR:SparkEngine>/Assets/${ASSET_DIR}"
            COMMENT "Copying ${ASSET_DIR} assets"
        )
    endif()
endforeach()

# Copy engine config files (settings.ini, controls.cfg)
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkEngine/Resources/Config")
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:SparkEngine>/Resources/Config"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/SparkEngine/Resources/Config"
            "$<TARGET_FILE_DIR:SparkEngine>/Resources/Config"
        COMMENT "Copying engine configuration files"
    )
endif()

# Copy libwinpthread-1.dll for MinGW cross-compilation (Wine needs it at runtime)
if(MINGW AND MINGW_WINPTHREAD_DLL)
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_WINPTHREAD_DLL}"
            "$<TARGET_FILE_DIR:SparkEngine>"
        COMMENT "Copying libwinpthread-1.dll for Wine runtime"
    )
endif()

# ---------------------------------------------------------------------
# 11.5) Precompiled Headers (PCH)
# ---------------------------------------------------------------------
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(SparkEngineLib PRIVATE
        <string>
        <vector>
        <memory>
        <unordered_map>
        <functional>
        <mutex>
        <cstdint>
        <algorithm>
        <sstream>
    )
    message(STATUS "Precompiled headers enabled")
endif()

# ---------------------------------------------------------------------
# 11.6) Include directories (on SparkEngineLib so game DLL inherits them)
# ---------------------------------------------------------------------
target_include_directories(SparkEngineLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/SparkEngine/Source>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/SparkSDK/Include>
    $<BUILD_INTERFACE:${SPARK_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/SparkEngine>
    $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty>
)
target_compile_features(SparkEngineLib PUBLIC cxx_std_23)
target_compile_definitions(SparkEngineLib PUBLIC
    $<$<PLATFORM_ID:Windows>:SPARK_PLATFORM_WINDOWS>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)
if(MSVC)
    # Public headers use C++20 variadic-macro features such as __VA_OPT__.
    # MSVC requires its conforming preprocessor mode even when the language
    # standard is published as C++23.
    target_compile_options(SparkEngineLib PUBLIC /Zc:preprocessor)
    # SparkEngine's Debug static ABI is built with iterator level 1. Publish
    # the same ABI setting so installed Debug consumers do not link a level-2
    # STL object model against the engine archives (LNK2038).
    target_compile_definitions(SparkEngineLib PUBLIC
        $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=1>
    )
endif()
if(SPARK_ANGELSCRIPT_AVAILABLE)
    target_include_directories(SparkEngineLib BEFORE PUBLIC
        $<BUILD_INTERFACE:${SPARK_ANGELSCRIPT_PATCH_DIR}>
    )
    target_compile_definitions(SparkEngineLib PUBLIC
        $<BUILD_INTERFACE:SPARK_ANGELSCRIPT_BUILD_TREE=1>
    )
endif()
foreach(_tp_dir ${THIRDPARTY_INCLUDE_DIRS})
    target_include_directories(SparkEngineLib PUBLIC
        $<BUILD_INTERFACE:${_tp_dir}>
    )
endforeach()

# Jolt Physics headers are added as SYSTEM to suppress third-party warnings
if(JOLT_FOUND)
    target_include_directories(SparkEngineLib SYSTEM PUBLIC
        $<BUILD_INTERFACE:${JOLT_PHYSICS_SOURCE_DIR}>
    )
endif()

# ---------------------------------------------------------------------
# 12) System libraries - Platform-specific (on SparkEngineLib)
# ---------------------------------------------------------------------
if(WIN32)
    set(SPARK_D3D_LIBS d3d11 dxgi d3dcompiler dxguid)
    if(NOT MINGW)
        list(APPEND SPARK_D3D_LIBS d3d12)
    endif()
    target_link_libraries(SparkEngineLib PUBLIC
        ${SPARK_D3D_LIBS}
        kernel32 user32 gdi32 winspool
        shell32 comdlg32 advapi32 bcrypt
        ole32 oleaut32 uuid
        winmm dbghelp xaudio2_8
        $<$<BOOL:${ENABLE_NETWORKING}>:ws2_32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wsock32>
        $<$<BOOL:${ENABLE_NETWORKING}>:crypt32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wldap32>
        $<$<BOOL:${ENABLE_NETWORKING}>:normaliz>
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(SparkEngineLib PUBLIC
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    if(APPLE)
        target_compile_definitions(SparkEngineLib PUBLIC SPARK_PLATFORM_MACOS)
    else()
        target_compile_definitions(SparkEngineLib PUBLIC SPARK_PLATFORM_LINUX)
    endif()

    # X11 for windowing and OpenGL context (GLX) — Linux only.
    # macOS has no X11 by default; Cocoa/CGL is used instead.
    if(NOT APPLE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            target_link_libraries(SparkEngineLib PUBLIC ${X11_LIBRARIES})
            target_include_directories(SparkEngineLib PUBLIC ${X11_INCLUDE_DIR})
            message(STATUS "X11 found - windowing support enabled")
        else()
            message(WARNING "X11 not found — falling back to headless mode only (install libx11-dev for windowed mode)")
        endif()
    endif()
endif()

if(MSVC)
    target_link_libraries(SparkEngineLib PUBLIC legacy_stdio_definitions)
endif()

# --- Vulkan backend libraries ---
if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkEngineLib PUBLIC Vulkan::Vulkan)
    else()
        target_include_directories(SparkEngineLib PUBLIC ${Vulkan_INCLUDE_DIR})
        target_link_libraries(SparkEngineLib PUBLIC ${Vulkan_LIBRARY})
    endif()
    message(STATUS "SparkEngine: Vulkan backend ENABLED")
endif()

# --- OpenGL backend libraries ---
if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_OPENGL_SUPPORT)
    target_link_libraries(SparkEngineLib PUBLIC OpenGL::GL)

    if(GLAD_FOUND)
        add_library(glad STATIC "${GLAD_SOURCE_DIR}/glad.c")
        target_include_directories(glad PUBLIC
            $<BUILD_INTERFACE:${GLAD_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include/SparkEngine/ThirdParty/glad>
        )
        if(MSVC)
            set_property(TARGET glad PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        target_link_libraries(SparkEngineLib PUBLIC glad)
    endif()

    # EGL for headless software rendering on Linux
    if(SPARK_EGL_AVAILABLE)
        target_compile_definitions(SparkEngineLib PUBLIC SPARK_EGL_SUPPORT)
        if(EGL_INCLUDE_DIR)
            target_include_directories(SparkEngineLib PUBLIC ${EGL_INCLUDE_DIR})
        endif()
        if(EGL_LIBRARY)
            target_link_libraries(SparkEngineLib PUBLIC ${EGL_LIBRARY})
        elseif(EGL_LIBRARIES)
            target_link_libraries(SparkEngineLib PUBLIC ${EGL_LIBRARIES})
        else()
            target_link_libraries(SparkEngineLib PUBLIC EGL)
        endif()
        message(STATUS "SparkEngine: EGL headless rendering ENABLED")
    endif()

    # GLX + X11 for Linux software rendering (via Xvfb when no display).
    # macOS uses CGL/NSOpenGL through Cocoa and has no X11 by default, so skip.
    if(NOT WIN32 AND NOT APPLE AND NOT SPARK_EGL_AVAILABLE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            target_link_libraries(SparkEngineLib PUBLIC X11::X11)
            # GLX is part of OpenGL on Linux — already linked via OpenGL::GL above
            message(STATUS "SparkEngine: GLX software rendering ENABLED (use Xvfb for headless)")
        endif()
    endif()

    message(STATUS "SparkEngine: OpenGL backend ENABLED")
endif()

# --- Metal backend ---
if(SPARK_METAL_AVAILABLE)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_METAL_SUPPORT)
    target_link_libraries(SparkEngineLib PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    message(STATUS "SparkEngine: Metal backend ENABLED")
endif()

# --- OpenAL audio backend (Linux/macOS) ---
if(SPARK_OPENAL_AVAILABLE)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_OPENAL_AVAILABLE=1)
    if(OPENAL_INCLUDE_DIR)
        target_include_directories(SparkEngineLib PUBLIC ${OPENAL_INCLUDE_DIR})
    endif()
    target_link_libraries(SparkEngineLib PUBLIC ${SPARK_OPENAL_LIBRARIES})
    message(STATUS "SparkEngine: OpenAL audio backend ENABLED")
endif()

# --- macOS platform frameworks and rpath ---
if(APPLE)
    # Cocoa is needed for windowing when SDL2 is not available; IOKit/CoreVideo
    # support HID and display link; AudioToolbox/CoreAudio are required by
    # OpenAL.framework when the system OpenAL fallback is used.
    target_link_libraries(SparkEngineLib PUBLIC
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    # Silence the blanket OpenGL deprecation warning: macOS caps system GL at
    # 4.1 but the backend still functions. MoltenVK/Metal are the long-term
    # path; until then, GL is the supported non-headless default.
    target_compile_definitions(SparkEngineLib PUBLIC GL_SILENCE_DEPRECATION=1)
    # Set rpath for .dylib game modules and shared libraries
    set(CMAKE_MACOSX_RPATH ON)
    set(CMAKE_INSTALL_RPATH "@executable_path;@executable_path/../lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    message(STATUS "SparkEngine: macOS frameworks linked (Cocoa, IOKit, CoreVideo, AudioToolbox, CoreAudio)")
endif()

# --- Neural Rendering ---
if(ENABLE_NEURAL_RENDERING)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_NEURAL_RENDERING=1)
    message(STATUS "SparkEngine: Neural Rendering ENABLED (NTC, radiance cache, neural post-processing)")
else()
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Neural/.*")
    message(STATUS "SparkEngine: Neural Rendering DISABLED")
endif()

# --- Hybrid Ray Tracing ---
if(ENABLE_HYBRID_RT)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HYBRID_RT=1)
    message(STATUS "SparkEngine: Hybrid Ray Tracing ENABLED (SDFGI software fallback)")
    if(ENABLE_DXR AND WIN32 AND NOT MINGW)
        target_compile_definitions(SparkEngineLib PUBLIC SPARK_HARDWARE_RT=1)
        message(STATUS "SparkEngine: Hardware RT (DXR) ENABLED")
    elseif(ENABLE_DXR AND (NOT WIN32 OR MINGW))
        message(STATUS "SparkEngine: DXR requested but requires D3D12 (Windows only) — SDFGI fallback active")
    endif()
endif()

# ---------------------------------------------------------------------
# 13) Third-party includes
# ---------------------------------------------------------------------

# ---------------------------------------------------------------------
# 14) Feature definitions
# ---------------------------------------------------------------------
set(FEATURE_DEFINITIONS "")

if(ENABLE_PROFILING)
    list(APPEND FEATURE_DEFINITIONS "PROFILING_ENABLED")
endif()

if(ENABLE_NETWORKING)
    list(APPEND FEATURE_DEFINITIONS "ENABLE_NETWORKING")

    # Enable crash report uploads when the earlier dependency probe succeeded.
    if(CURL_FOUND)
        list(APPEND FEATURE_DEFINITIONS "SPARK_HAS_CURL")
    endif()
endif()

if(SPARK_HEADLESS_SUPPORT)
    list(APPEND FEATURE_DEFINITIONS "SPARK_HEADLESS_SUPPORT")
endif()

if(ENABLE_VR)
    list(APPEND FEATURE_DEFINITIONS "SPARK_VR_ENABLED")
else()
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/VR/VRSystem\\.cpp$")
endif()

if(ENABLE_MOBILE)
    list(APPEND FEATURE_DEFINITIONS "SPARK_MOBILE_ENABLED")
else()
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Mobile/MobilePlatform\\.cpp$")
endif()





set(SPARK_PACKAGE_HAS_SDL2 0)
if(ENABLE_SDL2)
    # Prefer the ThirdParty submodule target, then system SDL2, then pkg-config
    if(TARGET SDL2::SDL2)
        list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
        set(SPARK_PACKAGE_HAS_SDL2 1)
        target_link_libraries(SparkEngineLib PUBLIC
            $<BUILD_INTERFACE:SDL2::SDL2>
            $<INSTALL_INTERFACE:SDL2::SDL2>
        )
        message(STATUS "SDL2 found and linked (windowing + input on Linux)")
    elseif(TARGET SDL2)
        list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
        set(SPARK_PACKAGE_HAS_SDL2 1)
        target_link_libraries(SparkEngineLib PUBLIC
            $<BUILD_INTERFACE:SDL2>
            $<INSTALL_INTERFACE:SDL2::SDL2>
        )
        # PUBLIC so the exe (SparkEngine.cpp) and tests see SDL.h; SPARK_SDL2_AVAILABLE
        # is PUBLIC too, so consumers need the include path.
        target_include_directories(SparkEngineLib PUBLIC
            $<BUILD_INTERFACE:$<TARGET_PROPERTY:SDL2,INTERFACE_INCLUDE_DIRECTORIES>>)
        message(STATUS "SDL2 linked from submodule (windowing + input on Linux)")
    else()
        find_package(SDL2 QUIET)
        if(SDL2_FOUND)
            list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
            if(TARGET SDL2::SDL2)
                set(SPARK_PACKAGE_HAS_SDL2 1)
                target_link_libraries(SparkEngineLib PUBLIC
                    $<BUILD_INTERFACE:SDL2::SDL2>
                    $<INSTALL_INTERFACE:SDL2::SDL2>
                )
            else()
                set(SPARK_PACKAGE_HAS_SDL2 1)
                target_include_directories(SparkEngineLib PUBLIC
                    $<BUILD_INTERFACE:${SDL2_INCLUDE_DIRS}>
                )
                target_link_libraries(SparkEngineLib PUBLIC
                    $<BUILD_INTERFACE:${SDL2_LIBRARIES}>
                    $<INSTALL_INTERFACE:SDL2::SDL2>
                )
            endif()
            message(STATUS "SDL2 found and linked (system install)")
        else()
            # Try pkg-config as a last resort
            find_package(PkgConfig QUIET)
            if(PKG_CONFIG_FOUND)
                pkg_check_modules(SDL2_PKG QUIET sdl2)
                if(SDL2_PKG_FOUND)
                    list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
                    set(SPARK_PACKAGE_HAS_SDL2 1)
                    target_include_directories(SparkEngineLib PUBLIC
                        $<BUILD_INTERFACE:${SDL2_PKG_INCLUDE_DIRS}>
                    )
                    target_link_libraries(SparkEngineLib PUBLIC
                        $<BUILD_INTERFACE:${SDL2_PKG_LIBRARIES}>
                        $<INSTALL_INTERFACE:SDL2::SDL2>
                    )
                    message(STATUS "SDL2 found via pkg-config and linked")
                else()
                    message(WARNING "SDL2 not found — run: git submodule update --init --recursive")
                endif()
            else()
                message(WARNING "SDL2 not found — run: git submodule update --init --recursive")
            endif()
        endif()
    endif()
endif()

# Build profile options — controls what ships in Shipping builds
if(ENABLE_CONSOLE_IN_SHIPPING)
    list(APPEND FEATURE_DEFINITIONS "SPARK_CONSOLE_IN_SHIPPING")
endif()
if(ENABLE_DEVCOMMANDS_IN_SHIPPING)
    list(APPEND FEATURE_DEFINITIONS "SPARK_DEVCOMMANDS_IN_SHIPPING")
endif()
if(STRIP_DEBUG_SYMBOLS)
    list(APPEND FEATURE_DEFINITIONS "SPARK_STRIP_DEBUG_SYMBOLS")
endif()

# Per-configuration build profile definitions
# These allow code to branch on build type at compile time:
#   SPARK_BUILD_DEBUG       — full debugging, assertions, unoptimized
#   SPARK_BUILD_DEVELOPMENT — symbols + optimization, console/profiler included
#   SPARK_BUILD_RELEASE     — optimized, optional console
#   SPARK_BUILD_SHIPPING    — maximum optimization, no dev tools
list(APPEND FEATURE_DEFINITIONS
    $<$<CONFIG:Debug>:SPARK_BUILD_DEBUG>
    $<$<CONFIG:RelWithDebInfo>:SPARK_BUILD_DEVELOPMENT>
    $<$<CONFIG:Release>:SPARK_BUILD_RELEASE>
    $<$<CONFIG:MinSizeRel>:SPARK_BUILD_SHIPPING>
)

# Apply feature definitions to the static library (inherited by exe and game DLL)
if(FEATURE_DEFINITIONS)
    target_compile_definitions(SparkEngineLib PUBLIC ${FEATURE_DEFINITIONS})
endif()

# ---------------------------------------------------------------------
# 14.5) SparkEngineInterface — defined earlier (section 9a.5)
#
# Moved above the game-module add_subdirectory() calls so each module's
# `if(TARGET SparkEngineInterface)` check evaluates true at configure
# time. See the block right after `add_library(SparkEngineLib ...)`.
# ---------------------------------------------------------------------

# ---------------------------------------------------------------------
# 15) Visual Studio settings
# ---------------------------------------------------------------------
if(MSVC)
    foreach(src ${SPARK_ENGINE_SOURCES})
        get_filename_component(dir "${src}" DIRECTORY)
        file(RELATIVE_PATH grp "${CMAKE_SOURCE_DIR}/SparkEngine/Source" "${dir}")
        string(REPLACE "/" "\\\\" grp "${grp}")
        if(grp STREQUAL "")
            source_group("Source Files" FILES "${src}")
        else()
            source_group("Source Files\\\\${grp}" FILES "${src}")
        endif()
    endforeach()

    # Set startup project to SparkEngine (the runtime host)
    set_property(TARGET SparkEngine PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SparkEngine)

    # Organize asset files in Visual Studio
    source_group("Assets\\\\Shaders" FILES "*.hlsl" "*.fx" "*.glsl")
    source_group("Assets\\\\Models" FILES "*.obj" "*.fbx" "*.dae" "*.3ds" "*.ply")
    source_group("Assets\\\\Textures" FILES "*.png" "*.jpg" "*.jpeg" "*.tga" "*.dds" "*.bmp")
    source_group("Assets\\\\Audio" FILES "*.wav" "*.mp3" "*.ogg" "*.flac")
    source_group("Assets\\\\Scripts" FILES "*.lua" "*.js" "*.py")
    source_group("Assets\\\\Config" FILES "*.ini" "*.json" "*.cfg" "*.xml" "*.properties")
endif()

# ---------------------------------------------------------------------
# 16) Test suite
# ---------------------------------------------------------------------
if(BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/Tests/CMakeLists.txt")
    enable_testing()
    add_test(
        NAME SparkWindowsVersionInfo.Metadata
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DSPARK_TEST_OUTPUT_DIR=${CMAKE_BINARY_DIR}/spark-version-info-test
            -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkWindowsVersionInfo.cmake)
    set_tests_properties(SparkWindowsVersionInfo.Metadata PROPERTIES
        LABELS "cmake;packaging")
    add_test(
        NAME SparkTrackedInstall.Contract
        COMMAND ${CMAKE_COMMAND}
            -DSPARK_TEST_ROOT=${CMAKE_BINARY_DIR}/spark-tracked-install-test
            -DSPARK_BINARY_ROOT=${CMAKE_BINARY_DIR}
            -DSPARK_TRACKED_INSTALL_HELPER=${CMAKE_SOURCE_DIR}/cmake/SparkTrackedInstall.cmake
            -P ${CMAKE_SOURCE_DIR}/cmake/TestSparkTrackedInstall.cmake)
    set_tests_properties(SparkTrackedInstall.Contract PROPERTIES
        LABELS "cmake;packaging;unit"
        TIMEOUT 30)
    message(STATUS "Adding test suite...")
    add_subdirectory(Tests)
endif()

# ---------------------------------------------------------------------
# 17) CMake install rules and find_package support
# ---------------------------------------------------------------------
# Allows standalone game projects to use:
#   find_package(SparkEngine REQUIRED)
#   spark_add_game_module(MyGame ${SOURCES})
#
# Install with: cmake --install build --prefix /path/to/install
# ---------------------------------------------------------------------

include("${CMAKE_SOURCE_DIR}/cmake/SparkTrackedInstall.cmake")

set(SPARK_INSTALL_COMPONENT_RUNTIME "runtime")
set(SPARK_INSTALL_COMPONENT_SDK "sdk")
set(SPARK_INSTALL_COMPONENT_TOOLS "tools")
set(SPARK_INSTALL_COMPONENT_TEMPLATES "templates")
set(SPARK_INSTALL_COMPONENT_SAMPLES "samples")

function(_spark_install_tracked_source_directory source_directory destination component)
    set(_spark_absolute_source "${source_directory}")
    cmake_path(ABSOLUTE_PATH _spark_absolute_source
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
        NORMALIZE)
    cmake_path(RELATIVE_PATH _spark_absolute_source
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _spark_relative_source)
    string(REPLACE "\\" "/" _spark_relative_source "${_spark_relative_source}")
    spark_install_tracked_directory(
        SOURCE "${_spark_relative_source}"
        DESTINATION "${destination}"
        COMPONENT "${component}"
        ${ARGN})
endfunction()

set(_SPARK_INSTALL_TARGETS SparkEngineLib)
foreach(_spark_dependency SparkAssetPipelineCore miniz tinyexr zstd tinyobjloader stb_image cgltf miniaudio nlohmann_json glad Jolt Recast Detour angelscript)
    if(TARGET ${_spark_dependency})
        list(APPEND _SPARK_INSTALL_TARGETS ${_spark_dependency})
    endif()
endforeach()
install(TARGETS ${_SPARK_INSTALL_TARGETS}
    EXPORT SparkEngineTargets
    ARCHIVE DESTINATION lib COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    LIBRARY DESTINATION lib COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
)
if(TARGET SparkEngineInterface)
    install(TARGETS SparkEngineInterface
        EXPORT SparkEngineTargets
        INCLUDES DESTINATION include
    )
endif()

# Keep the shipped Windows executable metadata in lockstep with
# SPARK_ENGINE_VERSION. The helper is idempotent because SparkBuild and
# SparkInstaller also attach their resources when configured standalone.
spark_attach_shipped_windows_version_info()

# Install root-owned SDK sources from a configure-time snapshot of the Git
# index, so local untracked headers cannot enter a package.
spark_install_tracked_directory(
    SOURCE SparkSDK/Include/Spark
    DESTINATION include/Spark
    COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
)
install(FILES "${SPARK_GENERATED_INCLUDE_DIR}/Spark/GeneratedVersion.h"
    DESTINATION include/Spark
    COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
)

if(TARGET SparkAssetPipelineCore)
    spark_install_tracked_directory(
        SOURCE SparkAssetPipelineCore/include
        DESTINATION include
        COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    )
endif()

# Install the engine's public headers (needed by modules that use engine types directly)
spark_install_tracked_directory(
    SOURCE SparkEngine/Source
    DESTINATION include/SparkEngine
    COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    INCLUDE_REGEXES "\\.(h|hpp)$"
)

# Install third-party headers that are part of the public interface
foreach(dir ${THIRDPARTY_INCLUDE_DIRS})
    if(EXISTS "${dir}")
        _spark_install_tracked_source_directory(
            "${dir}"
            include/SparkEngine/ThirdParty
            ${SPARK_INSTALL_COMPONENT_SDK}
            INCLUDE_REGEXES "\\.(h|hpp)$")
    endif()
endforeach()
if(SPARK_ANGELSCRIPT_AVAILABLE)
    # Keep the immutable upstream header at a non-colliding path and expose the
    # compatibility wrapper as the SDK's public <angelscript.h> entry point.
    _spark_install_tracked_source_directory(
        "${SPARK_ANGELSCRIPT_CORE_DIR}/include"
        include/SparkEngine/ThirdParty/angelscript_upstream
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.h$")
    _spark_install_tracked_source_directory(
        "${SPARK_ANGELSCRIPT_PATCH_DIR}"
        include/SparkEngine/ThirdParty
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.h$"
    )
endif()

install(EXPORT SparkEngineTargets
    FILE SparkEngineTargets.cmake
    NAMESPACE Spark::
    DESTINATION lib/cmake/SparkEngine
    COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
)

# Package config
include(CMakePackageConfigHelpers)

# The vendored shared SDL target is a public SDK dependency and a runtime DLL.
# Export it once, split by artifact kind, rather than layering duplicate install
# rules over SDL's upstream no-component installation.
if(TARGET SDL2 AND EXISTS "${SDL2_SUBMODULE_DIR}/include")
    install(TARGETS SDL2
        EXPORT SparkSDL2Targets
        RUNTIME DESTINATION bin COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
        LIBRARY DESTINATION lib COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
                NAMELINK_COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
        ARCHIVE DESTINATION lib COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
        FRAMEWORK DESTINATION lib COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
        INCLUDES DESTINATION include include/SDL2
    )
    install(EXPORT SparkSDL2Targets
        FILE SDL2Targets.cmake
        NAMESPACE SDL2::
        DESTINATION lib/cmake/SDL2
        COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    )
    install(FILES ${_SDL2_PUBLIC_HEADERS}
        "${CMAKE_BINARY_DIR}/ThirdParty/SDL2/include/SDL2/SDL_revision.h"
        "${CMAKE_BINARY_DIR}/ThirdParty/SDL2/include-config-$<LOWER_CASE:$<CONFIG>>/SDL2/SDL_config.h"
        DESTINATION include/SDL2
        COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    )
    if(MSVC)
        install(FILES "$<TARGET_PDB_FILE:SDL2>"
            DESTINATION bin
            COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
            OPTIONAL
        )
    endif()
    configure_package_config_file(
        "${CMAKE_SOURCE_DIR}/cmake/SparkSDL2Config.cmake.in"
        "${CMAKE_BINARY_DIR}/SDL2Config.cmake"
        INSTALL_DESTINATION lib/cmake/SDL2
    )
    install(FILES "${CMAKE_BINARY_DIR}/SDL2Config.cmake"
        DESTINATION lib/cmake/SDL2
        COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    )
endif()

set(SPARK_PACKAGE_HAS_CURL 0)
if(TARGET CURL::libcurl)
    set(SPARK_PACKAGE_HAS_CURL 1)
endif()
set(SPARK_PACKAGE_HAS_OPENAL 0)
if(SPARK_OPENAL_AVAILABLE AND TARGET OpenAL::OpenAL)
    # OpenAL::OpenAL is in SparkEngineLib's link interface, so installed
    # consumers must re-import it before SparkEngineTargets.cmake is loaded.
    set(SPARK_PACKAGE_HAS_OPENAL 1)
endif()
set(SPARK_PACKAGE_HAS_ANGELSCRIPT 0)
if(SPARK_ANGELSCRIPT_AVAILABLE)
    set(SPARK_PACKAGE_HAS_ANGELSCRIPT 1)
endif()
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/SparkEngineConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/SparkEngineConfig.cmake"
    INSTALL_DESTINATION lib/cmake/SparkEngine
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/SparkEngineGameModules.cmake.in"
    "${CMAKE_BINARY_DIR}/SparkEngineGameModules.cmake"
    @ONLY
    NEWLINE_STYLE LF
)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/SparkEngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_BINARY_DIR}/SparkEngineConfig.cmake"
    "${CMAKE_BINARY_DIR}/SparkEngineConfigVersion.cmake"
    "${CMAKE_BINARY_DIR}/SparkEngineGameModules.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/WriteSparkModuleABI.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/SparkPlugin.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/WriteSparkPluginMetadata.cmake"
    DESTINATION lib/cmake/SparkEngine
    COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
)

# Install the engine executable and every auto-discovered game module. Keeping
# this driven by the same target list used for the build prevents a newly added
# module from compiling successfully but silently disappearing from packages.
install(TARGETS SparkEngine
    RUNTIME DESTINATION bin COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
)
foreach(_SPARK_GAME_MODULE IN LISTS _SPARK_GAME_MODULE_NAMES)
    install(TARGETS ${_SPARK_GAME_MODULE}
        RUNTIME DESTINATION bin COMPONENT ${SPARK_INSTALL_COMPONENT_SAMPLES}
        LIBRARY DESTINATION bin COMPONENT ${SPARK_INSTALL_COMPONENT_SAMPLES}
    )
    install(FILES "$<TARGET_FILE:${_SPARK_GAME_MODULE}>.sparkabi"
        DESTINATION bin
        COMPONENT ${SPARK_INSTALL_COMPONENT_SAMPLES}
    )
endforeach()

if(TARGET SparkCrashReporter)
    install(TARGETS SparkCrashReporter
        RUNTIME DESTINATION bin COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
    )
endif()

# Match the normal post-build layout: engine HLSL files are flattened directly
# into Shaders/, while the repo-root shader tree remains under Shaders/HLSL/.
# Keeping the layouts distinct also avoids overwriting the differently authored
# BasicVS/DebugUtils/GaussianBlur/PBRSurface/Water variants shared by both trees.
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkEngine/Shaders/HLSL")
    spark_install_tracked_directory(
        SOURCE SparkEngine/Shaders/HLSL
        DESTINATION bin/Shaders
        COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
        INCLUDE_REGEXES "\\.hlsl$"
        FLATTEN
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkEngine/Shaders/Compiled")
    spark_install_tracked_directory(
        SOURCE SparkEngine/Shaders/Compiled
        DESTINATION bin/Shaders
        COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
        INCLUDE_REGEXES "\\.cso$"
        FLATTEN
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/Shaders")
    spark_install_tracked_directory(
        SOURCE Shaders
        DESTINATION bin/Shaders
        COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
    )
endif()

# Mirror the runtime asset set copied beside SparkEngine after a normal build.
# MMOFPS/Data is required for TerraFront module initialization and command
# registration, while Engine contains startup branding used before project load.
foreach(_spark_runtime_asset_dir Models Textures Audio Materials Scenes Scripts MMO MMOFPS Engine)
    if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/${_spark_runtime_asset_dir}")
        spark_install_tracked_directory(
            SOURCE "Assets/${_spark_runtime_asset_dir}"
            DESTINATION "bin/Assets/${_spark_runtime_asset_dir}"
            COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
        )
    endif()
endforeach()
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkEngine/Resources/Config")
    spark_install_tracked_directory(
        SOURCE SparkEngine/Resources/Config
        DESTINATION bin/Resources/Config
        COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME}
    )
endif()

# Install tools
if(TARGET SparkShaderCompiler)
    install(TARGETS SparkShaderCompiler
        RUNTIME DESTINATION bin COMPONENT ${SPARK_INSTALL_COMPONENT_TOOLS}
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/Tools")
    spark_install_tracked_directory(
        SOURCE Tools
        DESTINATION tools
        COMPONENT ${SPARK_INSTALL_COMPONENT_TOOLS}
        EXCLUDE_REGEXES
            "/__pycache__(/|$)"
            "\\.py[co]$"
            "/tests(/|$)"
    )
endif()

# Install project templates and samples
if(EXISTS "${CMAKE_SOURCE_DIR}/Templates")
    spark_install_tracked_directory(
        SOURCE Templates
        DESTINATION share/SparkEngine/templates
        COMPONENT ${SPARK_INSTALL_COMPONENT_TEMPLATES}
        EXCLUDE_REGEXES
            "/(build[^/]*|dist|Binaries|Intermediate|Saved|Saves|Logs|Screenshots|TestScreenshots|Temp|[.]vs|[.]cache|CMakeFiles|__pycache__|[.]pytest_cache)(/|$)"
            "/(CMakeCache[.]txt|cmake_install[.]cmake|CTestTestfile[.]cmake|compile_commands[.]json|install_manifest[.]txt|[.]ninja_deps|[.]ninja_log|exec_audit[.]log|spark_trace[.]json|mmo_data[.]db)$"
            "[.]py[co]$"
            "[.]user$"
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/Samples")
    spark_install_tracked_directory(
        SOURCE Samples
        DESTINATION share/SparkEngine/samples
        COMPONENT ${SPARK_INSTALL_COMPONENT_SAMPLES}
    )
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/GameModules/SparkGame")
    spark_install_tracked_directory(
        SOURCE GameModules/SparkGame
        DESTINATION share/SparkEngine/samples/SparkGame
        COMPONENT ${SPARK_INSTALL_COMPONENT_SAMPLES}
        INCLUDE_REGEXES
            "\\.(h|hpp|cpp|json)$"
            "/CMakeLists[.]txt$"
    )
endif()

message(STATUS "CMake install rules configured (use 'cmake --install' for find_package support)")

# ---------------------------------------------------------------------
# 18) Build summary
# ---------------------------------------------------------------------
message(STATUS "=== SPARK ENGINE + GAME CONFIGURED SUCCESSFULLY ===")
if(MSVC)
    message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} (MSVC VS ${SPARK_VS_YEAR} / ${SPARK_VS_TOOLSET})")
else()
    message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
endif()
message(STATUS "Output: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "Graphics Backends:")
if(WIN32 AND MINGW)
    message(STATUS "  DirectX 11 : ENABLED (MinGW cross-compilation — run under Wine + DXVK)")
    message(STATUS "  DirectX 12 : EXCLUDED (MinGW SDK headers lack required D3D12/DXR interfaces)")
elseif(WIN32)
    message(STATUS "  DirectX 11 : ENABLED (Windows)")
else()
    message(STATUS "  DirectX 11 : DISABLED (non-Windows)")
endif()
if(SPARK_VULKAN_AVAILABLE)
    message(STATUS "  Vulkan     : ENABLED")
else()
    message(STATUS "  Vulkan     : DISABLED")
endif()
if(SPARK_OPENGL_AVAILABLE)
    message(STATUS "  OpenGL     : ENABLED")
else()
    message(STATUS "  OpenGL     : DISABLED")
endif()
message(STATUS "SparkEngine: EXECUTABLE (runtime host)")
if(BUILD_GAME_MODULES AND _SPARK_GAME_MODULE_NAMES)
    string(REPLACE ";" ", " _MOD_LIST "${_SPARK_GAME_MODULE_NAMES}")
    message(STATUS "Game Modules: ${_MOD_LIST}")
elseif(BUILD_GAME_MODULES)
    message(STATUS "Game Modules: none found in GameModules/")
else()
    message(STATUS "Game Modules: OFF (engine-only build)")
endif()
message(STATUS "Editor: ${ENABLE_EDITOR}")
message(STATUS "Launcher: ${ENABLE_LAUNCHER}")
message(STATUS "SparkBuild: ${ENABLE_SPARKBUILD}")
message(STATUS "Shader Compiler Tool: ENABLED")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Networking: ${ENABLE_NETWORKING}")
message(STATUS "  DXR RayTracing : ${ENABLE_DXR}")
message(STATUS "  SDL2 Input     : ${ENABLE_SDL2}")
if(SPARK_ANGELSCRIPT_AVAILABLE)
    message(STATUS "  AngelScript    : ENABLED")
elseif(ENABLE_ANGELSCRIPT)
    message(STATUS "  AngelScript    : MISSING SDK (unsupported contract)")
else()
    message(STATUS "  AngelScript    : DISABLED (unsupported contract)")
endif()
message(STATUS "  Headless Mode  : ${SPARK_HEADLESS_SUPPORT}")
message(STATUS "  VR Framework   : ${ENABLE_VR}")
message(STATUS "  Mobile Platform: ${ENABLE_MOBILE}")

# ---------------------------------------------------------------------
# 19) Third-party dependency status — detailed diagnostics
#
# Every dependency reports:  status, what it provides, what breaks
# without it, and exactly how to install it.  This lets users and CI
# logs diagnose issues without searching external docs.
# ---------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== THIRD-PARTY DEPENDENCY STATUS ===")
message(STATUS "")

set(_SPARK_MISSING_DEPS "")
set(_SPARK_MISSING_COUNT 0)

# --- Helper macro: prints the "how to get it" block for a missing dep ---
macro(_spark_dep_help _name)
    message(STATUS "  ${_name}:")
    # Remaining arguments are passed via ARGN; each is one STATUS line
    foreach(_line ${ARGN})
        message(STATUS "    ${_line}")
    endforeach()
endmacro()

# ---- Core engine libraries (git submodules) ----

# Jolt Physics
if(JOLT_FOUND)
    message(STATUS "  [OK]      Jolt Physics    — rigid body physics, collision, raycasting")
else()
    list(APPEND _SPARK_MISSING_DEPS "JoltPhysics")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] Jolt Physics    — rigid body physics, collision, raycasting")
    _spark_dep_help("Jolt Physics"
        "What:    Full 3D physics engine (rigid bodies, collision detection, raycasting, character controller)"
        "Impact:  PhysicsSystem disabled — engine uses inert stub (no collisions, no gravity, no raycasts)"
        "Install: git submodule update --init ThirdParty/Physics/JoltPhysics"
        "Docs:    https://github.com/jrouwe/JoltPhysics"
    )
endif()

# miniz
if(MINIZ_FOUND)
    message(STATUS "  [OK]      miniz           — crash dump compression, save file compression")
else()
    list(APPEND _SPARK_MISSING_DEPS "miniz")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] miniz           — crash dump compression, save file compression")
    _spark_dep_help("miniz"
        "What:    Lightweight zlib-compatible compression library (single-file)"
        "Impact:  CrashHandler disabled (stub used), save files are uncompressed"
        "Install: git submodule update --init ThirdParty/Utils/miniz"
        "Docs:    https://github.com/richgel999/miniz"
    )
endif()

# zstd
if(TARGET zstd)
    message(STATUS "  [OK]      zstd            — Zstandard compression (fast decompression)")
else()
    message(STATUS "  [STUB]    zstd            — Zstandard compression (using stub)")
endif()

# VMA
if(TARGET VulkanMemoryAllocator)
    message(STATUS "  [OK]      VMA             — Vulkan Memory Allocator (GPU memory management)")
else()
    message(STATUS "  [STUB]    VMA             — Vulkan Memory Allocator (using stub)")
endif()

# EnTT
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include/entt/entt.hpp")
    message(STATUS "  [OK]      EnTT            — entity component system")
else()
    list(APPEND _SPARK_MISSING_DEPS "entt")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [STUB]    EnTT            — entity component system (using minimal stub)")
    _spark_dep_help("EnTT"
        "What:    Header-only ECS framework — manages all game entities and components"
        "Impact:  Using minimal stub — no archetype storage, no groups, no signals, limited ECS features"
        "Install: git submodule update --init ThirdParty/ECS/entt"
        "Docs:    https://github.com/skypjack/entt"
    )
endif()

# Dear ImGui
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui/imgui.cpp")
    message(STATUS "  [OK]      Dear ImGui      — editor UI, debug overlays")
else()
    list(APPEND _SPARK_MISSING_DEPS "imgui")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] Dear ImGui      — editor UI, debug overlays")
    _spark_dep_help("Dear ImGui"
        "What:    Immediate-mode GUI library for the editor and in-game debug overlays"
        "Impact:  SparkEditor cannot build — no debug panels, no property inspector, no console overlay"
        "Install: git submodule update --init ThirdParty/UI/imgui"
        "Docs:    https://github.com/ocornut/imgui"
    )
endif()

# AngelScript
if(SPARK_ANGELSCRIPT_AVAILABLE)
    message(STATUS "  [OK]      AngelScript     — hot-reload scripting")
elseif(NOT ENABLE_ANGELSCRIPT)
    message(STATUS "  [OFF]     AngelScript     — disabled by ENABLE_ANGELSCRIPT=OFF")
    _spark_dep_help("AngelScript"
        "What:    Statically-typed scripting language with C++-like syntax and hot-reload"
        "Impact:  SparkGameVisualScript rejects load and registers no vs_* commands"
        "Enable:  configure with -DENABLE_ANGELSCRIPT=ON"
    )
else()
    list(APPEND _SPARK_MISSING_DEPS "angelscript")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] AngelScript     — hot-reload scripting")
    _spark_dep_help("AngelScript"
        "What:    Statically-typed scripting language with C++-like syntax and hot-reload"
        "Impact:  Scripting is unavailable; SparkGameVisualScript rejects load and registers no vs_* commands"
        "Install: git submodule update --init ThirdParty/Scripting/angelscript-mirror"
        "Docs:    https://www.angelcode.com/angelscript/"
    )
endif()

# TinyObjLoader
if(TINYOBJ_FOUND)
    message(STATUS "  [OK]      TinyObjLoader   — OBJ model loading")
else()
    list(APPEND _SPARK_MISSING_DEPS "tinyobjloader")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] TinyObjLoader   — OBJ model loading")
    _spark_dep_help("TinyObjLoader"
        "What:    Lightweight Wavefront OBJ/MTL mesh loader"
        "Impact:  Cannot load .obj model files — other formats (FBX, glTF) unaffected"
        "Install: git submodule update --init ThirdParty/Utils/tinyobjloader"
        "Docs:    https://github.com/tinyobjloader/tinyobjloader"
    )
endif()

# Recast / Detour
if(RECAST_FOUND)
    message(STATUS "  [OK]      Recast/Detour   — navmesh generation and pathfinding")
else()
    if(ENABLE_RECAST)
        list(APPEND _SPARK_MISSING_DEPS "recast")
        math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
        message(STATUS "  [MISSING] Recast/Detour   — navmesh generation and pathfinding")
        _spark_dep_help("Recast/Detour"
            "What:    Navigation mesh generation (Recast) and pathfinding (Detour) for AI agents"
            "Impact:  AI agents cannot navigate the world — NavMeshSystem disabled"
            "Install: git submodule update --init ThirdParty/AI/recastnavigation"
            "Docs:    https://github.com/recastnavigation/recastnavigation"
        )
    else()
        message(STATUS "  [OFF]     Recast/Detour   — disabled by ENABLE_RECAST=OFF")
    endif()
endif()

# ---- Platform / system libraries ----

message(STATUS "")
message(STATUS "  --- Platform Libraries ---")

# SDL2
if(ENABLE_SDL2)
    if(SDL2_FOUND OR SDL2_PKG_FOUND OR TARGET SDL2::SDL2 OR TARGET SDL2)
        message(STATUS "  [OK]      SDL2            — cross-platform windowing and input")
    else()
        list(APPEND _SPARK_MISSING_DEPS "SDL2")
        math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
        message(STATUS "  [MISSING] SDL2            — cross-platform windowing and input")
        _spark_dep_help("SDL2"
            "What:    Windowing, input, and audio abstraction layer (required for Linux desktop mode)"
            "Impact:  No windowed mode on Linux — engine runs headless only"
            "Install: git submodule update --init ThirdParty/SDL2"
            "  Or install the system package:"
            "  Ubuntu/Debian : sudo apt install libsdl2-dev"
            "  Fedora        : sudo dnf install SDL2-devel"
            "  Arch          : sudo pacman -S sdl2"
            "  macOS         : brew install sdl2"
            "  Windows       : vcpkg install sdl2 (or download from https://libsdl.org)"
            "Docs:    https://wiki.libsdl.org/SDL2"
        )
    endif()
else()
    message(STATUS "  [OFF]     SDL2            — disabled (ENABLE_SDL2=OFF)")
endif()

# Vulkan
if(ENABLE_VULKAN)
    if(SPARK_VULKAN_AVAILABLE)
        message(STATUS "  [OK]      Vulkan SDK      — Vulkan graphics backend")
    else()
        message(STATUS "  [MISSING] Vulkan SDK      — Vulkan graphics backend")
        _spark_dep_help("Vulkan SDK"
            "What:    Low-level cross-platform graphics API (experimental backend)"
            "Impact:  Vulkan renderer unavailable — D3D11 (Windows) or OpenGL (Linux) still work"
            "Install:"
            "  All platforms : https://vulkan.lunarg.com/sdk/home"
            "  Ubuntu/Debian : sudo apt install libvulkan-dev vulkan-tools"
            "  Fedora        : sudo dnf install vulkan-devel vulkan-tools"
            "  macOS         : brew install vulkan-sdk (MoltenVK)"
            "  Tip: Set VULKAN_SDK env var if installed to a non-standard path"
        )
    endif()
else()
    message(STATUS "  [OFF]     Vulkan SDK      — disabled (ENABLE_VULKAN=OFF)")
endif()

# OpenGL + GLAD
if(ENABLE_OPENGL)
    if(SPARK_OPENGL_AVAILABLE)
        message(STATUS "  [OK]      OpenGL + GLAD   — OpenGL graphics backend")
    else()
        if(NOT OpenGL_FOUND)
            message(STATUS "  [MISSING] OpenGL          — OpenGL graphics backend")
            _spark_dep_help("OpenGL"
                "What:    Cross-platform graphics API (experimental backend)"
                "Impact:  OpenGL renderer unavailable"
                "Install:"
                "  Ubuntu/Debian : sudo apt install libgl-dev mesa-common-dev"
                "  Fedora        : sudo dnf install mesa-libGL-devel"
                "  Arch          : sudo pacman -S mesa"
                "  macOS         : Ships with Xcode (OpenGL is deprecated on macOS)"
                "  Windows       : Ships with GPU drivers — ensure drivers are up to date"
            )
        endif()
        if(NOT GLAD_FOUND)
            message(STATUS "  [MISSING] GLAD            — OpenGL function loader")
            _spark_dep_help("GLAD"
                "What:    OpenGL extension loader — loads GL function pointers at runtime"
                "Impact:  OpenGL backend cannot initialize without a loader"
                "Install: git submodule update --init ThirdParty/Graphics/glad"
                "  Or generate from: https://glad.dav1d.de/ (select GL 4.6 Core, C/C++)"
            )
        endif()
    endif()
else()
    message(STATUS "  [OFF]     OpenGL          — disabled (ENABLE_OPENGL=OFF)")
endif()

# X11 (Linux windowing)
if(NOT WIN32 AND NOT APPLE)
    if(X11_FOUND)
        message(STATUS "  [OK]      X11             — Linux windowing and display")
    else()
        message(STATUS "  [MISSING] X11             — Linux windowing and display")
        _spark_dep_help("X11"
            "What:    X Window System client libraries for window creation and input"
            "Impact:  No native window creation on Linux (headless mode only unless SDL2 is used)"
            "Install:"
            "  Ubuntu/Debian : sudo apt install libx11-dev"
            "  Fedora        : sudo dnf install libX11-devel"
            "  Arch          : sudo pacman -S libx11"
        )
    endif()
endif()

# libcurl (optional, for crash report uploading)
if(ENABLE_NETWORKING)
    if(CURL_FOUND)
        message(STATUS "  [OK]      libcurl         — HTTP client (crash report uploads)")
    else()
        message(STATUS "  [SKIP]    libcurl         — HTTP client (optional)")
        _spark_dep_help("libcurl"
            "What:    HTTP/HTTPS client library for uploading crash reports"
            "Impact:  Crash reports are saved locally but not uploaded to a server"
            "Install:"
            "  Ubuntu/Debian : sudo apt install libcurl4-openssl-dev"
            "  Fedora        : sudo dnf install libcurl-devel"
            "  Arch          : sudo pacman -S curl"
            "  macOS         : brew install curl"
            "  Windows       : vcpkg install curl"
        )
    endif()
endif()

# ---- Summary ----

message(STATUS "")
if(_SPARK_MISSING_COUNT GREATER 0)
    string(REPLACE ";" ", " _SPARK_MISSING_LIST "${_SPARK_MISSING_DEPS}")
    message(WARNING
        "${_SPARK_MISSING_COUNT} dependency(ies) not found: ${_SPARK_MISSING_LIST}\n"
        "\n"
        "  The engine will build but with DEGRADED functionality.\n"
        "  Most submodule dependencies can be fixed with:\n"
        "\n"
        "    git submodule update --init --recursive\n"
        "\n"
        "  System libraries (SDL2, Vulkan, X11, etc.) must be installed via your\n"
        "  system package manager — see the per-dependency messages above.\n"
        "\n"
        "  For full details, see README.md 'Dependencies' section.\n"
        "  To make missing deps a hard error: cmake -DSPARK_STRICT_DEPS=ON ..."
    )

    # Strict mode: fail the build if critical deps are missing
    if(SPARK_STRICT_DEPS)
        if(NOT JOLT_FOUND)
            message(FATAL_ERROR "SPARK_STRICT_DEPS: Jolt Physics is required but not found. "
                "Run: git submodule update --init ThirdParty/Physics/JoltPhysics")
        endif()
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui/imgui.cpp")
            message(FATAL_ERROR "SPARK_STRICT_DEPS: Dear ImGui is required but not found. "
                "Run: git submodule update --init ThirdParty/UI/imgui")
        endif()
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include/entt/entt.hpp")
            message(FATAL_ERROR "SPARK_STRICT_DEPS: EnTT is required but not found. "
                "Run: git submodule update --init ThirdParty/ECS/entt")
        endif()
    endif()
else()
    message(STATUS "All dependencies found. Full engine functionality available.")
endif()
message(STATUS "")

# ---------------------------------------------------------------------
# CPack packaging configuration
# ---------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "SparkEngine")
set(CPACK_PACKAGE_VENDOR "Spark Engine Team")
set(CPACK_PACKAGE_CONTACT "https://github.com/Krilliac/SparkEngine")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Krilliac/SparkEngine")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SparkEngine C++23 game engine SDK/runtime distribution")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SparkEngine")

set(CPACK_GENERATOR "ZIP")
if(WIN32)
    list(APPEND CPACK_GENERATOR "NSIS" "WIX")
else()
    list(APPEND CPACK_GENERATOR "TGZ")
endif()

set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_COMPONENTS_ALL
    ${SPARK_INSTALL_COMPONENT_RUNTIME}
    ${SPARK_INSTALL_COMPONENT_SDK}
    ${SPARK_INSTALL_COMPONENT_TOOLS}
    ${SPARK_INSTALL_COMPONENT_TEMPLATES}
    ${SPARK_INSTALL_COMPONENT_SAMPLES}
)

set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Runtime Binaries")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "SparkEngine runtime executable, runtime shaders, and required binaries")
set(CPACK_COMPONENT_RUNTIME_REQUIRED ON)

set(CPACK_COMPONENT_SDK_DISPLAY_NAME "Headers and SDK")
set(CPACK_COMPONENT_SDK_DESCRIPTION "SparkEngine static library, CMake package files, and public headers")
set(CPACK_COMPONENT_SDK_DEPENDS runtime)

set(CPACK_COMPONENT_TOOLS_DISPLAY_NAME "Developer Tools")
set(CPACK_COMPONENT_TOOLS_DESCRIPTION "Command-line tools such as spark-cli and SparkShaderCompiler")
set(CPACK_COMPONENT_TOOLS_DEPENDS runtime)

set(CPACK_COMPONENT_TEMPLATES_DISPLAY_NAME "Project Templates")
set(CPACK_COMPONENT_TEMPLATES_DESCRIPTION "Starter templates for new SparkEngine projects")
set(CPACK_COMPONENT_TEMPLATES_DEPENDS sdk)

set(CPACK_COMPONENT_SAMPLES_DISPLAY_NAME "Sample Content")
set(CPACK_COMPONENT_SAMPLES_DESCRIPTION "Reference sample modules and sample project content")
set(CPACK_COMPONENT_SAMPLES_DEPENDS runtime)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    message(FATAL_ERROR "SparkEngine packaging requires the repository LICENSE file")
endif()
# WiX rejects license resources without a recognized extension. Keep the
# repository's canonical LICENSE file extensionless, but stage an identical
# .txt copy for all CPack generators and ship it in every runtime payload.
configure_file(
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_BINARY_DIR}/SparkEngineLicense.txt"
    COPYONLY
)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/SparkEngineLicense.txt")
install(FILES "${CMAKE_BINARY_DIR}/SparkEngineLicense.txt"
    DESTINATION .
    RENAME LICENSE.txt
    COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME})
install(FILES "${SPARK_THIRD_PARTY_NOTICES_FILE}"
    DESTINATION .
    RENAME THIRD_PARTY_NOTICES.txt
    COMPONENT ${SPARK_INSTALL_COMPONENT_RUNTIME})
if(EXISTS "${CMAKE_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
endif()

set(CPACK_SOURCE_GENERATOR "ZIP;TGZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/build/"
    "/[.]git/"
    "/[.]claude/"
    "/[.]vs/"
    "/[.]vscode/"
    "/out/"
)
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)

if(TARGET miniz)
    _spark_install_tracked_source_directory(
        "${MINIZ_ROOT}"
        include/SparkEngine/ThirdParty/miniz
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.h$")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h"
        DESTINATION include/SparkEngine/ThirdParty/miniz
        COMPONENT ${SPARK_INSTALL_COMPONENT_SDK}
    )
endif()
if(TARGET tinyexr)
    _spark_install_tracked_source_directory(
        "${TINYEXR_DIR}"
        include/SparkEngine/ThirdParty/tinyexr
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.(h|hh)$")
endif()
if(TARGET glad)
    _spark_install_tracked_source_directory(
        "${GLAD_INCLUDE_DIR}"
        include/SparkEngine/ThirdParty/glad
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.h$")
endif()
foreach(_spark_header_install
        "${ZSTD_ROOT}|include/SparkEngine/ThirdParty/zstd"
        "${TINYOBJLOADER_INCLUDE_DIR}|include/SparkEngine/ThirdParty/tinyobjloader"
        "${STB_DIR}|include/SparkEngine/ThirdParty/stb"
        "${CGLTF_DIR}|include/SparkEngine/ThirdParty/cgltf"
        "${MINIAUDIO_DIR}|include/SparkEngine/ThirdParty/miniaudio"
        "${NLOHMANN_JSON_DIR}|include/SparkEngine/ThirdParty/json")
    string(REPLACE "|" ";" _spark_header_parts "${_spark_header_install}")
    list(GET _spark_header_parts 0 _spark_header_source)
    list(GET _spark_header_parts 1 _spark_header_destination)
    if(EXISTS "${_spark_header_source}")
        _spark_install_tracked_source_directory(
            "${_spark_header_source}"
            "${_spark_header_destination}"
            ${SPARK_INSTALL_COMPONENT_SDK}
            INCLUDE_REGEXES "\\.(h|hpp)$")
    endif()
endforeach()
if(TARGET Jolt)
    _spark_install_tracked_source_directory(
        "${JOLT_PHYSICS_SOURCE_DIR}/Jolt"
        include/Jolt
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.(h|inl)$")
endif()
if(TARGET Recast)
    _spark_install_tracked_source_directory(
        "${RECAST_DIR}/Recast/Include"
        include/SparkEngine/ThirdParty/recast/Recast
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.h$")
endif()
if(TARGET Detour)
    _spark_install_tracked_source_directory(
        "${RECAST_DIR}/Detour/Include"
        include/SparkEngine/ThirdParty/recast/Detour
        ${SPARK_INSTALL_COMPONENT_SDK}
        INCLUDE_REGEXES "\\.h$")
endif()
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/SparkCPackOptions.cmake")

include(CPack)
