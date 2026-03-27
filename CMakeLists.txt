cmake_minimum_required(VERSION 3.25)

# ================================================================
# Modern CMake Configuration - Cross-Platform Build
# ================================================================

# All targets need PIC since game modules are shared libraries that link SparkEngineLib
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Set CMake policies for modern behavior
cmake_policy(SET CMP0091 NEW)  # Enable MSVC_RUNTIME_LIBRARY support

# Note: CMAKE_MSVC_RUNTIME_LIBRARY is set after project() below,
# because the MSVC variable is not defined until the compiler is identified.

# MSVC toolset selection:
#   v143 = Visual Studio 2022
#   v144 = Visual Studio 2026
# Override at configure time: cmake -DSPARK_MSVC_TOOLSET=v144 ...
set(SPARK_MSVC_TOOLSET "" CACHE STRING
    "MSVC toolset version (e.g. v143 for VS 2022, v144 for VS 2026). Empty = auto-detect.")

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
# Platform-specific generator settings (only if not already set)
if(WIN32 AND NOT CMAKE_GENERATOR_TOOLSET)
    if(NOT SPARK_MSVC_TOOLSET STREQUAL "")
        set(CMAKE_GENERATOR_TOOLSET "${SPARK_MSVC_TOOLSET}")
    else()
        # Default to v143 (VS 2022). Use -DSPARK_MSVC_TOOLSET=v144 for VS 2026.
        set(CMAKE_GENERATOR_TOOLSET "v143")
    endif()
endif()
if(WIN32 AND NOT CMAKE_GENERATOR_PLATFORM)
    set(CMAKE_GENERATOR_PLATFORM "x64")
endif()

project(SparkEngine LANGUAGES CXX C)

# Set consistent runtime library for all targets using modern CMake (CMP0091)
# Must be after project() so that the MSVC variable is defined.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------
# 2.5) Compiler identification and version reporting
# ---------------------------------------------------------------------
if(MSVC)
    # Map _MSC_VER ranges to Visual Studio release years
    if(MSVC_VERSION GREATER_EQUAL 1950)
        set(SPARK_VS_YEAR "2026+")
        set(SPARK_VS_TOOLSET "v144+")
    elseif(MSVC_VERSION GREATER_EQUAL 1940)
        set(SPARK_VS_YEAR "2026")
        set(SPARK_VS_TOOLSET "v144")
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
option(ENABLE_METAL "Enable Metal graphics backend (macOS, experimental)" OFF)
option(ENABLE_DXR "Enable DirectX Raytracing support (experimental)" OFF)
option(ENABLE_HYBRID_RT "Enable hybrid ray tracing (SDFGI software + optional hardware DXR/Vulkan RT)" ON)
# SDL2 is auto-enabled on Linux for windowing + input; can be used on Windows too
if(NOT WIN32)
    option(ENABLE_SDL2 "Enable SDL2 cross-platform input and windowing" ON)
else()
    option(ENABLE_SDL2 "Enable SDL2 cross-platform input and windowing" OFF)
endif()
option(SPARK_HEADLESS_SUPPORT "Enable headless/dedicated server mode support" ON)
option(ENABLE_RECAST "Enable Recast/Detour navmesh generation (improved voxel-based builder)" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_GAME_MODULES "Build in-tree game modules (SparkGameFPS, SparkGameMMO, etc.)" ON)
option(SPARK_STRICT_DEPS "FATAL_ERROR on missing critical dependencies (Jolt, ImGui, EnTT)" OFF)
option(SPARK_SUPPRESS_THIRDPARTY_WARNINGS "Suppress compiler warnings from third-party libraries (Jolt, etc.)" ON)
option(SPARK_DOUBLE_PRECISION_PHYSICS "Enable double precision physics (JPH_DOUBLE_PRECISION) for large worlds" OFF)

# Editor available on Windows (Win32+DX11) and Linux (SDL2+OpenGL3)
option(ENABLE_EDITOR "Enable editor features" ON)

# Build profile options — controls what ships in the final binary
option(ENABLE_CONSOLE_IN_SHIPPING "Include SparkConsole in Shipping builds" OFF)
option(ENABLE_DEVCOMMANDS_IN_SHIPPING "Include developer commands (god, noclip, etc.) in Shipping builds" OFF)
option(STRIP_DEBUG_SYMBOLS "Strip debug symbols from the final binary" OFF)

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

# Make sure all configurations use the same output directory
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# ---------------------------------------------------------------------
# 6) Compiler options (platform-specific)
# ---------------------------------------------------------------------
if(MSVC)
    add_compile_options(
        /W3 /MP /bigobj
        /Zi          # Always generate PDB debug info (enables stack trace symbol resolution)
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

    # Conformance improvements available in VS 2017+ - enforce __cplusplus macro and
    # disable non-conforming extensions so code is portable across compilers.
    if(MSVC_VERSION GREATER_EQUAL 1910)
        add_compile_options(/Zc:__cplusplus /permissive-)
    endif()

    # VS 2026 (v144, _MSC_VER >= 1940): enable C++26 readiness flags
    if(MSVC_VERSION GREATER_EQUAL 1940)
        add_compile_options(
            /Zc:preprocessor   # Standard-conforming preprocessor (fully enabled in v144)
            /wd5045            # Spectre mitigation informational warning
        )
        add_compile_definitions(SPARK_COMPILER_VS2026)
    # VS 2022 (v143, _MSC_VER 1930-1939)
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
    )
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
# 7) Third-party library configuration
# ---------------------------------------------------------------------

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
    target_include_directories(miniz PUBLIC "${MINIZ_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}")

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
        $<INSTALL_INTERFACE:include>
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

# Header-only third-party libraries (cross-platform)
set(THIRDPARTY_INCLUDE_DIRS "")

# EnTT (ECS) - prefer the real library, fall back to minimal stub
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include/entt/entt.hpp")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
    message(STATUS "Found EnTT")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt_stub/entt/entt.hpp")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt_stub")
    message(STATUS "Found EnTT (minimal stub - initialize submodule for full library)")
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

# AngelScript
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Scripting/angelscript-mirror/angelscript/include")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/Scripting/angelscript-mirror/angelscript/include")
    message(STATUS "Found AngelScript")
endif()

# ---------------------------------------------------------------------
# 7.5) Graphics API backend detection (Vulkan, OpenGL)
# ---------------------------------------------------------------------

# --- Vulkan ---
if(ENABLE_VULKAN)
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
    find_package(OpenGL QUIET)
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
    set(SPARK_METAL_AVAILABLE TRUE)
    message(STATUS "Metal backend enabled (experimental)")
else()
    set(SPARK_METAL_AVAILABLE FALSE)
    if(ENABLE_METAL AND NOT APPLE)
        message(STATUS "Metal backend requires macOS — disabled on this platform")
    else()
        message(STATUS "Metal backend disabled by option")
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
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*/[Tt]est[^a-z].*|.*/[Tt]ests/.*")
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# Separate entry point from library sources
# SparkEngine.cpp contains wWinMain/main - it goes in the exe only
set(SPARK_ENGINE_ENTRY_POINT "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngine.cpp")
set(SPARK_ENGINE_LIB_SOURCES ${SPARK_ENGINE_SOURCES})
list(REMOVE_ITEM SPARK_ENGINE_LIB_SOURCES "${SPARK_ENGINE_ENTRY_POINT}")

# Exclude source files whose third-party dependencies are missing
if(NOT JOLT_FOUND)
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystem\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystemQueries\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsShapeFactory\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsBodyImpl\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsConsoleOps\\.cpp$")
    message(STATUS "Jolt Physics not found - using PhysicsSystemStub.cpp")
else()
    # When the real PhysicsSystem is built, exclude the stub
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystemStub\\.cpp$")
endif()
if(NOT MINIZ_FOUND)
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashHandler\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashHandlerHelpers\\.cpp$")
    message(STATUS "MINIZ not found - excluding CrashHandler*.cpp, using CrashHandlerStub.cpp")
else()
    # When the real CrashHandler is built, exclude the stub
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Utils/CrashHandlerStub\\.cpp$")
endif()

# 9a) SparkEngineLib - Static library with all engine systems
add_library(SparkEngineLib STATIC ${SPARK_ENGINE_LIB_SOURCES})
set_target_properties(SparkEngineLib PROPERTIES OUTPUT_NAME "SparkEngineLib")

# 9b) SparkEngine - Executable (entry point + module loader)
if(WIN32)
    add_executable(SparkEngine WIN32 "${SPARK_ENGINE_ENTRY_POINT}")
else()
    add_executable(SparkEngine "${SPARK_ENGINE_ENTRY_POINT}")
endif()
target_link_libraries(SparkEngine PRIVATE SparkEngineLib)

# On Linux/macOS, export the executable's symbols so that game module .so files
# loaded via dlopen() can resolve SparkEngineLib symbols from the host process.
# Without this, each .so would need its own copy of SparkEngineLib, creating
# duplicate singletons (NetworkManager, EngineContext, etc.) and ABI mismatches.
if(NOT WIN32)
    set_target_properties(SparkEngine PROPERTIES ENABLE_EXPORTS ON)
endif()

# Link third-party libraries to the static lib (both exe and game DLL get them)
if(MINIZ_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE miniz)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_MINIZ_AVAILABLE=1)
    target_compile_definitions(SparkEngine PRIVATE SPARK_MINIZ_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with MINIZ")
endif()

if(CURL_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE CURL::libcurl)
    message(STATUS "Linking SparkEngineLib with libcurl")
endif()

if(TINYOBJ_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE tinyobjloader)
    message(STATUS "Linking SparkEngineLib with TinyObjLoader")
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
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
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
if(ENABLE_RECAST AND EXISTS "${RECAST_DIR}/Recast/Source/Recast.cpp")
    file(GLOB RECAST_SOURCES "${RECAST_DIR}/Recast/Source/*.cpp")
    add_library(Recast STATIC ${RECAST_SOURCES})
    target_include_directories(Recast PUBLIC "${RECAST_DIR}/Recast/Include")
    if(MSVC)
        target_compile_options(Recast PRIVATE /W0)
    else()
        target_compile_options(Recast PRIVATE -w)
    endif()

    file(GLOB DETOUR_SOURCES "${RECAST_DIR}/Detour/Source/*.cpp")
    add_library(Detour STATIC ${DETOUR_SOURCES})
    target_include_directories(Detour PUBLIC "${RECAST_DIR}/Detour/Include")
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
# 9.5) Build game modules from GameModules/ directory (auto-discovery)
#
# Drop a game module folder into GameModules/ with a CMakeLists.txt and
# it will be built automatically.  Remove the folder to exclude it.
# Set BUILD_GAME_MODULES=OFF to skip all game modules entirely.
# ---------------------------------------------------------------------
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
                COMMENT "Copying ${_MOD_NAME} DLL to engine output directory"
                VERBATIM
            )
            list(APPEND _SPARK_GAME_MODULE_NAMES "${_MOD_NAME}")
        endif()
    endforeach()
else()
    message(STATUS "Game modules: SKIPPED (BUILD_GAME_MODULES=OFF)")
endif()

# ---------------------------------------------------------------------
# 9.6) Build SparkEditor if enabled and exists
# ---------------------------------------------------------------------
set(SPARK_EDITOR_DEPS_OK TRUE)
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui/imgui.h")
    set(SPARK_EDITOR_DEPS_OK FALSE)
    set(SPARK_EDITOR_SKIP_REASON "Dear ImGui not found (initialize submodule)")
elseif(NOT WIN32)
    # Linux editor requires SDL2 and OpenGL — check before adding subdirectory
    find_package(SDL2 QUIET)
    find_package(OpenGL QUIET)
    if(NOT SDL2_FOUND)
        set(SPARK_EDITOR_DEPS_OK FALSE)
        set(SPARK_EDITOR_SKIP_REASON "SDL2 not found (install libsdl2-dev)")
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

# ---------------------------------------------------------------------
# 10) Asset Management - Copy shaders and assets to engine output
# ---------------------------------------------------------------------
add_custom_command(TARGET SparkEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Assets"
    COMMENT "Creating engine asset directory structure"
)

# Copy HLSL shaders
file(GLOB_RECURSE ENGINE_SHADER_FILES "${CMAKE_SOURCE_DIR}/SparkEngine/Shaders/HLSL/*.hlsl")
foreach(SHADER_FILE ${ENGINE_SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADER_FILE}
            "$<TARGET_FILE_DIR:SparkEngine>/Shaders/${SHADER_NAME}"
        COMMENT "Copying shader ${SHADER_NAME}"
    )
endforeach()

# Copy model assets
if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/Models")
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets/Models"
            "$<TARGET_FILE_DIR:SparkEngine>/Assets/Models"
        COMMENT "Copying model assets"
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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/GameModules/SparkGameFPS/Source>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/SparkSDK/Include>
    $<INSTALL_INTERFACE:include>
)
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
    target_link_libraries(SparkEngineLib PUBLIC
        d3d11 dxgi d3dcompiler dxguid
        kernel32 user32 gdi32 winspool
        shell32 comdlg32 advapi32
        ole32 oleaut32 uuid
        winmm
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

    # X11 for windowing and OpenGL context (GLX)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_link_libraries(SparkEngineLib PUBLIC ${X11_LIBRARIES})
        target_include_directories(SparkEngineLib PUBLIC ${X11_INCLUDE_DIR})
        message(STATUS "X11 found - windowing support enabled")
    else()
        message(STATUS "X11 not found - headless mode only")
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
        target_include_directories(glad PUBLIC "${GLAD_INCLUDE_DIR}")
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

    # GLX + X11 for Linux software rendering (via Xvfb when no display)
    if(NOT WIN32 AND NOT SPARK_EGL_AVAILABLE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            target_link_libraries(SparkEngineLib PUBLIC X11::X11)
            target_link_libraries(SparkEngineLib PUBLIC GLX)
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

# --- Hybrid Ray Tracing ---
if(ENABLE_HYBRID_RT)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_HYBRID_RT=1)
    message(STATUS "SparkEngine: Hybrid Ray Tracing ENABLED (SDFGI software fallback)")
    if(ENABLE_DXR)
        target_compile_definitions(SparkEngineLib PUBLIC SPARK_HARDWARE_RT=1)
        message(STATUS "SparkEngine: Hardware RT (DXR) ENABLED")
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

    # Check if libcurl is available for crash report uploading
    find_package(CURL QUIET)
    if(CURL_FOUND)
        list(APPEND FEATURE_DEFINITIONS "SPARK_HAS_CURL")
    endif()
endif()

if(SPARK_HEADLESS_SUPPORT)
    list(APPEND FEATURE_DEFINITIONS "SPARK_HEADLESS_SUPPORT")
endif()

if(ENABLE_SDL2)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
        if(TARGET SDL2::SDL2)
            target_link_libraries(SparkEngineLib PUBLIC SDL2::SDL2)
        else()
            # Fallback for older FindSDL2 modules that don't provide imported targets
            target_include_directories(SparkEngineLib PUBLIC ${SDL2_INCLUDE_DIRS})
            target_link_libraries(SparkEngineLib PUBLIC ${SDL2_LIBRARIES})
        endif()
        message(STATUS "SDL2 found and linked (windowing + input on Linux)")
    else()
        # Try pkg-config as a last resort (common on many Linux distros)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SDL2_PKG QUIET sdl2)
            if(SDL2_PKG_FOUND)
                list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
                target_include_directories(SparkEngineLib PUBLIC ${SDL2_PKG_INCLUDE_DIRS})
                target_link_libraries(SparkEngineLib PUBLIC ${SDL2_PKG_LIBRARIES})
                message(STATUS "SDL2 found via pkg-config and linked")
            else()
                message(WARNING "SDL2 not found - Linux windowed mode requires SDL2. Install libsdl2-dev.")
            endif()
        else()
            message(WARNING "SDL2 not found - Linux windowed mode requires SDL2. Install libsdl2-dev.")
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
# 14.5) SparkEngineInterface — header-only view of SparkEngineLib
#
# On Linux, game module .so files must NOT link SparkEngineLib's object
# code, otherwise each .so gets its own copy of every singleton and the
# class layout diverges when conditional members (#ifdef ENABLE_NETWORKING)
# change member offsets.  Instead, game modules link this INTERFACE
# target, which provides the same include directories and compile
# definitions but zero object code.  Undefined symbols are resolved at
# dlopen() time from the host executable (which has ENABLE_EXPORTS ON).
# ---------------------------------------------------------------------
if(NOT WIN32)
    add_library(SparkEngineInterface INTERFACE)
    target_include_directories(SparkEngineInterface INTERFACE
        $<TARGET_PROPERTY:SparkEngineLib,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(SparkEngineInterface INTERFACE
        $<TARGET_PROPERTY:SparkEngineLib,INTERFACE_COMPILE_DEFINITIONS>)
endif()

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

set(_SPARK_INSTALL_TARGETS SparkEngineLib)
if(TARGET tinyobjloader)
    list(APPEND _SPARK_INSTALL_TARGETS tinyobjloader)
endif()
install(TARGETS ${_SPARK_INSTALL_TARGETS}
    EXPORT SparkEngineTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# Install SparkSDK public headers
install(DIRECTORY SparkSDK/Include/Spark
    DESTINATION include
)

# Install the engine's public headers (needed by modules that use engine types directly)
install(DIRECTORY "SparkEngine/Source/"
    DESTINATION include/SparkEngine
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
)

# Install third-party headers that are part of the public interface
foreach(dir ${THIRDPARTY_INCLUDE_DIRS})
    if(EXISTS "${dir}")
        install(DIRECTORY "${dir}/"
            DESTINATION include/SparkEngine/ThirdParty
            FILES_MATCHING
                PATTERN "*.h"
                PATTERN "*.hpp"
        )
    endif()
endforeach()

# Export targets (only when all dependencies are in export sets)
if(FALSE) # Disabled: third-party targets (Jolt, miniz) lack EXPORT sets
install(EXPORT SparkEngineTargets
    FILE SparkEngineTargets.cmake
    NAMESPACE Spark::
    DESTINATION lib/cmake/SparkEngine
)
endif()

# Package config
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/SparkEngineConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/SparkEngineConfig.cmake"
    INSTALL_DESTINATION lib/cmake/SparkEngine
)

install(FILES
    "${CMAKE_BINARY_DIR}/SparkEngineConfig.cmake"
    "${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake"
    DESTINATION lib/cmake/SparkEngine
)

# Install engine executable and game DLL
install(TARGETS SparkEngine
    RUNTIME DESTINATION bin
)
if(TARGET SparkGameFPS)
    install(TARGETS SparkGameFPS
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
    )
endif()

# Install shaders
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkEngine/Shaders")
    install(DIRECTORY "SparkEngine/Shaders/"
        DESTINATION bin/Shaders
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
if(WIN32)
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
message(STATUS "Shader Compiler Tool: ENABLED")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Networking: ${ENABLE_NETWORKING}")
message(STATUS "  DXR RayTracing : ${ENABLE_DXR}")
message(STATUS "  SDL2 Input     : ${ENABLE_SDL2}")
message(STATUS "  Headless Mode  : ${SPARK_HEADLESS_SUPPORT}")

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
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Scripting/angelscript-mirror/angelscript/include")
    message(STATUS "  [OK]      AngelScript     — hot-reload scripting")
else()
    list(APPEND _SPARK_MISSING_DEPS "angelscript")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] AngelScript     — hot-reload scripting")
    _spark_dep_help("AngelScript"
        "What:    Statically-typed scripting language with C++-like syntax and hot-reload"
        "Impact:  Scripting system unavailable — no gameplay scripts, no hot-reload, no script console"
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
    if(SDL2_FOUND OR SDL2_PKG_FOUND)
        message(STATUS "  [OK]      SDL2            — cross-platform windowing and input")
    else()
        list(APPEND _SPARK_MISSING_DEPS "SDL2")
        math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
        message(STATUS "  [MISSING] SDL2            — cross-platform windowing and input")
        _spark_dep_help("SDL2"
            "What:    Windowing, input, and audio abstraction layer (required for Linux desktop mode)"
            "Impact:  No windowed mode on Linux — engine runs headless only"
            "Install:"
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
