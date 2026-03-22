cmake_minimum_required(VERSION 3.25)

# ================================================================
# Modern CMake Configuration - Cross-Platform Build
# ================================================================

# All targets need PIC since SparkGame is a shared library that links SparkEngineLib
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
option(ENABLE_NETWORKING "Enable networking features (experimental — requires socket libs)" OFF)
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
option(SPARK_STRICT_DEPS "FATAL_ERROR on missing critical dependencies (Bullet3, ImGui, EnTT)" OFF)

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
        /wd4005  # Macro redefinition
        /wd4996  # Deprecated functions
        /wd4244  # Conversion warnings
        /wd4267  # size_t conversion warnings
        /wd26495 # Uninitialized member variable warnings (for TinyObjLoader)
    )
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

# Bullet Physics
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/bullet3/src")
    set(BULLET_PHYSICS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/bullet3/src")
    set(BULLET_FOUND TRUE)
    message(STATUS "Found Bullet Physics")
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
                message(STATUS "Vulkan SDK path set but libraries not found")
            endif()
        else()
            set(SPARK_VULKAN_AVAILABLE FALSE)
            message(STATUS "Vulkan not found - Vulkan backend disabled")
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
#   SparkGame       = SHARED LIBRARY / DLL (game code, links SparkEngineLib)
#
# The engine exe loads SparkGame.dll at runtime via GameModuleLoader.
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
if(NOT BULLET_FOUND)
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystem\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsSystemQueries\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsShapeFactory\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsBodyImpl\\.cpp$")
    list(FILTER SPARK_ENGINE_LIB_SOURCES EXCLUDE REGEX ".*/Physics/PhysicsConsoleOps\\.cpp$")
    message(STATUS "Bullet Physics not found - using PhysicsSystemStub.cpp")
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

# Link third-party libraries to the static lib (both exe and game DLL get them)
if(MINIZ_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE miniz)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_MINIZ_AVAILABLE=1)
    target_compile_definitions(SparkEngine PRIVATE SPARK_MINIZ_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with MINIZ")
endif()

if(TINYOBJ_FOUND)
    target_link_libraries(SparkEngineLib PRIVATE tinyobjloader)
    message(STATUS "Linking SparkEngineLib with TinyObjLoader")
endif()

# Link Bullet Physics
if(BULLET_FOUND)
    set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/ThirdParty/Physics/bullet3" "${CMAKE_BINARY_DIR}/bullet3")
    # Suppress warnings in third-party bullet3 code (deprecated-copy triggers on GCC 12+)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM")
        foreach(_bullet_target
            BulletDynamics BulletCollision LinearMath
            BulletSoftBody BulletInverseDynamics
            Bullet3Common Bullet3Collision Bullet3Dynamics Bullet3Geometry
            Bullet3OpenCL_clew Bullet2FileLoader
        )
            if(TARGET ${_bullet_target})
                target_compile_options(${_bullet_target} PRIVATE
                    -Wno-deprecated-copy
                    -Wno-implicit-fallthrough
                    -Wno-unused-but-set-variable
                )
                if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                    target_compile_options(${_bullet_target} PRIVATE -Wno-volatile)
                elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|IntelLLVM")
                    target_compile_options(${_bullet_target} PRIVATE -Wno-deprecated-volatile)
                endif()
                if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|IntelLLVM")
                    target_compile_options(${_bullet_target} PRIVATE
                        -Wno-single-bit-bitfield-constant-conversion
                        -Wno-null-pointer-subtraction
                    )
                endif()
            endif()
        endforeach()
    endif()
    target_link_libraries(SparkEngineLib PRIVATE BulletDynamics BulletCollision LinearMath)
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_BULLET_PHYSICS_AVAILABLE=1)
    target_compile_definitions(SparkEngine PRIVATE SPARK_BULLET_PHYSICS_AVAILABLE=1)
    message(STATUS "Linking SparkEngineLib with Bullet Physics")
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
# 9.5) Build SparkGame (game module DLL loaded by engine at runtime)
# ---------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkGame/CMakeLists.txt")
    message(STATUS "Adding SparkGame game module to build...")
    add_subdirectory(SparkGame)

    if(TARGET SparkGame)
        # SparkEngineLib (static) contains SparkConsole code that references
        # Game/Player symbols exported from SparkGame.  Link SparkEngine
        # against SparkGame's import library so the linker can resolve them.
        target_link_libraries(SparkEngine PRIVATE SparkGame)
        # Re-add SparkEngineLib so the linker can resolve symbols that
        # SparkGame.so/.dll left undefined (e.g. CrashHandler functions
        # referenced by SparkConsole code linked into SparkGame).
        target_link_libraries(SparkEngine PRIVATE SparkEngineLib)
        message(STATUS "SparkEngine will link SparkGame import library")

        # Copy SparkGame DLL next to the engine executable after build.
        # This is done here (on SparkEngine) rather than as a SparkGame
        # post-build step to avoid a circular build dependency.
        add_custom_command(TARGET SparkEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SparkGame>
                $<TARGET_FILE_DIR:SparkEngine>
            COMMENT "Copying SparkGame DLL to engine output directory"
            VERBATIM
        )
    endif()
endif()

# ---------------------------------------------------------------------
# 9.55) Build SparkGameMMO (MMO networking showcase module DLL)
# ---------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/SparkGameMMO/CMakeLists.txt")
    message(STATUS "Adding SparkGameMMO game module to build...")
    add_subdirectory(SparkGameMMO)

    if(TARGET SparkGameMMO)
        # Copy SparkGameMMO DLL next to the engine executable after build.
        add_custom_command(TARGET SparkEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SparkGameMMO>
                $<TARGET_FILE_DIR:SparkEngine>
            COMMENT "Copying SparkGameMMO DLL to engine output directory"
            VERBATIM
        )
    endif()
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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/SparkGame/Source>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/SparkSDK/Include>
    $<INSTALL_INTERFACE:include>
)
foreach(_tp_dir ${THIRDPARTY_INCLUDE_DIRS})
    target_include_directories(SparkEngineLib PUBLIC
        $<BUILD_INTERFACE:${_tp_dir}>
    )
endforeach()

# Bullet Physics headers are added as SYSTEM to suppress third-party warnings
if(BULLET_FOUND)
    target_include_directories(SparkEngineLib SYSTEM PUBLIC
        $<BUILD_INTERFACE:${BULLET_PHYSICS_SOURCE_DIR}>
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
    list(APPEND FEATURE_DEFINITIONS "NETWORKING_ENABLED")
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
if(FALSE) # Disabled: third-party targets (Bullet, miniz) lack EXPORT sets
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
if(TARGET SparkGame)
    install(TARGETS SparkGame
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
message(STATUS "SparkGame: SHARED LIBRARY (game module DLL)")
message(STATUS "Editor: ${ENABLE_EDITOR}")
message(STATUS "Shader Compiler Tool: ENABLED")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Networking: ${ENABLE_NETWORKING}")
message(STATUS "  DXR RayTracing : ${ENABLE_DXR}")
message(STATUS "  SDL2 Input     : ${ENABLE_SDL2}")
message(STATUS "  Headless Mode  : ${SPARK_HEADLESS_SUPPORT}")

# ---------------------------------------------------------------------
# 19) Third-party dependency status and missing module warnings
# ---------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== THIRD-PARTY DEPENDENCY STATUS ===")

set(_SPARK_MISSING_DEPS "")
set(_SPARK_MISSING_COUNT 0)

# Bullet Physics
if(BULLET_FOUND)
    message(STATUS "  [OK]      Bullet Physics  — rigid body physics, collision, raycasting")
else()
    list(APPEND _SPARK_MISSING_DEPS "bullet3")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] Bullet Physics  — PhysicsSystem disabled, no rigid body simulation")
endif()

# miniz
if(MINIZ_FOUND)
    message(STATUS "  [OK]      miniz           — crash dump compression, save file compression")
else()
    list(APPEND _SPARK_MISSING_DEPS "miniz")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] miniz           — CrashHandler disabled (using stub), no save compression")
endif()

# EnTT
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include/entt/entt.hpp")
    message(STATUS "  [OK]      EnTT            — entity component system")
else()
    list(APPEND _SPARK_MISSING_DEPS "entt")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [STUB]    EnTT            — using minimal stub, ECS features limited")
endif()

# Dear ImGui
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui/imgui.cpp")
    message(STATUS "  [OK]      Dear ImGui      — editor UI, debug overlays")
else()
    list(APPEND _SPARK_MISSING_DEPS "imgui")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] Dear ImGui      — editor and debug UI unavailable")
endif()

# AngelScript
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Scripting/angelscript-mirror/angelscript/include")
    message(STATUS "  [OK]      AngelScript     — hot-reload scripting")
else()
    list(APPEND _SPARK_MISSING_DEPS "angelscript")
    math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
    message(STATUS "  [MISSING] AngelScript     — scripting system unavailable")
endif()

# TinyObjLoader
if(TINYOBJ_FOUND)
    message(STATUS "  [OK]      TinyObjLoader   — OBJ model loading")
else()
    message(STATUS "  [MISSING] TinyObjLoader   — OBJ model loading unavailable")
endif()

# SDL2 (Linux)
if(ENABLE_SDL2)
    if(SDL2_FOUND OR SDL2_PKG_FOUND)
        message(STATUS "  [OK]      SDL2            — cross-platform windowing and input")
    else()
        list(APPEND _SPARK_MISSING_DEPS "SDL2")
        math(EXPR _SPARK_MISSING_COUNT "${_SPARK_MISSING_COUNT} + 1")
        message(STATUS "  [MISSING] SDL2            — no windowed mode on Linux (install libsdl2-dev)")
    endif()
endif()

# Print warnings if dependencies are missing
if(_SPARK_MISSING_COUNT GREATER 0)
    message(STATUS "")
    message(WARNING
        "${_SPARK_MISSING_COUNT} third-party dependency(ies) not found: ${_SPARK_MISSING_DEPS}\n"
        "  The engine will build but with DEGRADED functionality.\n"
        "  To fix: initialize git submodules and install system packages:\n"
        "    git submodule update --init --recursive\n"
        "    # Linux: sudo apt install libsdl2-dev    (for SDL2)\n"
        "  See README.md 'Dependencies' section for details."
    )

    # Strict mode: fail the build if critical deps are missing
    if(SPARK_STRICT_DEPS)
        if(NOT BULLET_FOUND)
            message(FATAL_ERROR "SPARK_STRICT_DEPS: Bullet Physics is required but not found. "
                "Run: git submodule update --init ThirdParty/Physics/bullet3")
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
    message(STATUS "")
    message(STATUS "All third-party dependencies found. Full engine functionality available.")
endif()
message(STATUS "")
