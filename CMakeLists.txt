cmake_minimum_required(VERSION 3.16)

# ================================================================
# Modern CMake Configuration - Cross-Platform Build
# ================================================================

# All targets need PIC since SparkGame is a shared library that links SparkEngineLib
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Set CMake policies for modern behavior
cmake_policy(SET CMP0091 NEW)  # Enable MSVC_RUNTIME_LIBRARY support

# Set consistent runtime library for all targets using modern CMake (CMP0091)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # Note: With CMP0091 NEW, CMAKE_MSVC_RUNTIME_LIBRARY handles /MD and /MDd
    # automatically. Do NOT manually add /MD or /MDd flags as they conflict.
endif()

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
option(ENABLE_PHYSX "Enable PhysX physics engine" ON)
option(ENABLE_LUA "Enable Lua scripting support" ON)
option(ENABLE_COLLABORATIVE "Enable collaborative features" ON)
option(ENABLE_GRAPHICS "Enable graphics engine" ON)
option(ENABLE_PROFILING "Enable profiling tools" ON)
option(ENABLE_NETWORKING "Enable networking features" OFF)  # Disabled for now to avoid CURL issues
option(ENABLE_ASSET_STREAMING "Enable asset streaming" ON)
option(ENABLE_HOT_RELOAD "Enable hot reload functionality" ON)
option(ENABLE_TERRAIN_SYSTEM "Enable terrain system" ON)
option(ENABLE_POST_PROCESSING "Enable post-processing effects" ON)
option(ENABLE_LIGHTING_SYSTEM "Enable advanced lighting" ON)
option(ENABLE_ADVANCED_INPUT "Enable advanced input features" ON)
option(ENABLE_VULKAN "Enable Vulkan graphics backend" ON)
option(ENABLE_OPENGL "Enable OpenGL graphics backend" ON)
option(ENABLE_AI "Enable AI and navigation systems" ON)
option(ENABLE_ANIMATION "Enable skeletal animation system" ON)
option(ENABLE_SAVE_SYSTEM "Enable save/load system" ON)
option(ENABLE_PROCEDURAL "Enable procedural generation" ON)
option(ENABLE_CINEMATIC "Enable cinematic sequencer" ON)
option(ENABLE_DECALS "Enable decal system" ON)
option(ENABLE_MESH_LOD "Enable mesh LOD system" ON)
option(ENABLE_DXR "Enable DirectX Raytracing support" OFF)  # Requires D3D12, disabled by default
option(ENABLE_SDL2 "Enable SDL2 cross-platform input" OFF)  # Requires SDL2 library
option(ENABLE_WEATHER "Enable weather system" ON)
option(ENABLE_INVENTORY "Enable inventory system" ON)
option(ENABLE_QUEST_SYSTEM "Enable quest/objective system" ON)
option(ENABLE_EVENT_SYSTEM "Enable publish/subscribe event bus" ON)
option(ENABLE_DAY_NIGHT "Enable day/night cycle system" ON)
option(ENABLE_PERF_STATS "Enable performance statistics overlay" ON)
option(ENABLE_FOG_SYSTEM "Enable fog rendering system" ON)
option(ENABLE_SCREEN_SPACE "Enable screen-space effects (SSAO, SSR)" ON)
option(BUILD_TESTS "Build test suite" ON)

# Editor only available on Windows (requires Win32 + DX11 ImGui backends)
if(WIN32)
    option(ENABLE_EDITOR "Enable editor features" ON)
else()
    set(ENABLE_EDITOR OFF)
    message(STATUS "SparkEditor disabled on non-Windows platform (requires Win32+DX11)")
endif()

# ---------------------------------------------------------------------
# 4) C++ standard and in-source build guard
# ---------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
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

    # VS 2026 (v144, _MSC_VER >= 1940): enable additional C++23 readiness flags
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

# EnTT (ECS)
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/ECS/entt/single_include")
    message(STATUS "Found EnTT")
endif()

# Bullet Physics
if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/bullet3/src")
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/bullet3/src")
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

# ---------------------------------------------------------------------
# 8) Build SparkConsole FIRST to ensure it's available for SparkEngine
# ---------------------------------------------------------------------
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/SparkConsole/CMakeLists.txt")
    message(STATUS "Building SparkConsole first...")
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
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*[Tt]est.*")
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# Separate entry point from library sources
# SparkEngine.cpp contains wWinMain/main - it goes in the exe only
set(SPARK_ENGINE_ENTRY_POINT "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Core/SparkEngine.cpp")
set(SPARK_ENGINE_LIB_SOURCES ${SPARK_ENGINE_SOURCES})
list(REMOVE_ITEM SPARK_ENGINE_LIB_SOURCES "${SPARK_ENGINE_ENTRY_POINT}")

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
    target_link_libraries(SparkEngineLib PRIVATE BulletDynamics BulletCollision LinearMath)
    message(STATUS "Linking SparkEngineLib with Bullet Physics")
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
# 9.6) Build SparkEditor if enabled and exists
# ---------------------------------------------------------------------
if(ENABLE_EDITOR AND EXISTS "${CMAKE_SOURCE_DIR}/SparkEditor/CMakeLists.txt")
    message(STATUS "Adding SparkEditor to build...")
    add_subdirectory(SparkEditor)

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
    target_compile_definitions(SparkEngineLib PUBLIC SPARK_PLATFORM_LINUX)

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

# ---------------------------------------------------------------------
# 13) Lua, PhysX, and other third-party includes
# ---------------------------------------------------------------------
if(ENABLE_LUA)
    foreach(PATH IN ITEMS
        "ThirdParty/Scripting/lua"
        "ThirdParty/Scripting/sol2/single/include"
        "ThirdParty/sol2/single/include"
    )
        if(EXISTS "${CMAKE_SOURCE_DIR}/${PATH}")
            if(PATH MATCHES "sol2")
                add_library(sol2 INTERFACE)
                target_include_directories(sol2 INTERFACE "${CMAKE_SOURCE_DIR}/${PATH}")
                target_link_libraries(SparkEngineLib PUBLIC sol2)
                target_compile_definitions(SparkEngineLib PUBLIC LUA_SCRIPTING_ENABLED)
                message(STATUS "Found Sol2 at: ${PATH}")
            else()
                target_include_directories(SparkEngineLib PUBLIC "${CMAKE_SOURCE_DIR}/${PATH}")
                message(STATUS "Found Lua headers at: ${PATH}")
            endif()
        endif()
    endforeach()
endif()

if(ENABLE_PHYSX)
    if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/PhysX/include")
        target_include_directories(SparkEngineLib PUBLIC "ThirdParty/Physics/PhysX/include")
        target_compile_definitions(SparkEngineLib PUBLIC PHYSX_AVAILABLE)
        message(STATUS "PhysX headers found")
    else()
        message(STATUS "PhysX not found (optional)")
    endif()
endif()

# ---------------------------------------------------------------------
# 14) Feature definitions
# ---------------------------------------------------------------------
set(FEATURE_DEFINITIONS "")

if(ENABLE_EDITOR)
    list(APPEND FEATURE_DEFINITIONS "EDITOR_ENABLED")
endif()

if(ENABLE_GRAPHICS)
    list(APPEND FEATURE_DEFINITIONS "GRAPHICS_ENABLED")
endif()

if(ENABLE_COLLABORATIVE)
    list(APPEND FEATURE_DEFINITIONS "COLLABORATIVE_ENABLED")
endif()

if(ENABLE_PROFILING)
    list(APPEND FEATURE_DEFINITIONS "PROFILING_ENABLED")
endif()

if(ENABLE_NETWORKING)
    list(APPEND FEATURE_DEFINITIONS "NETWORKING_ENABLED")
endif()

if(ENABLE_ASSET_STREAMING)
    list(APPEND FEATURE_DEFINITIONS "ASSET_STREAMING_ENABLED")
endif()

if(ENABLE_HOT_RELOAD)
    list(APPEND FEATURE_DEFINITIONS "HOT_RELOAD_ENABLED")
endif()

if(ENABLE_TERRAIN_SYSTEM)
    list(APPEND FEATURE_DEFINITIONS "TERRAIN_SYSTEM_ENABLED")
endif()

if(ENABLE_POST_PROCESSING)
    list(APPEND FEATURE_DEFINITIONS "POST_PROCESSING_ENABLED")
endif()

if(ENABLE_LIGHTING_SYSTEM)
    list(APPEND FEATURE_DEFINITIONS "LIGHTING_SYSTEM_ENABLED")
endif()

if(ENABLE_ADVANCED_INPUT)
    list(APPEND FEATURE_DEFINITIONS "ADVANCED_INPUT_ENABLED")
endif()

if(SPARK_VULKAN_AVAILABLE)
    list(APPEND FEATURE_DEFINITIONS "VULKAN_BACKEND_AVAILABLE")
endif()

if(SPARK_OPENGL_AVAILABLE)
    list(APPEND FEATURE_DEFINITIONS "OPENGL_BACKEND_AVAILABLE")
endif()

if(ENABLE_AI)
    list(APPEND FEATURE_DEFINITIONS "AI_SYSTEM_ENABLED")
endif()

if(ENABLE_ANIMATION)
    list(APPEND FEATURE_DEFINITIONS "ANIMATION_SYSTEM_ENABLED")
endif()

if(ENABLE_SAVE_SYSTEM)
    list(APPEND FEATURE_DEFINITIONS "SAVE_SYSTEM_ENABLED")
endif()

if(ENABLE_PROCEDURAL)
    list(APPEND FEATURE_DEFINITIONS "PROCEDURAL_GENERATION_ENABLED")
endif()

if(ENABLE_CINEMATIC)
    list(APPEND FEATURE_DEFINITIONS "CINEMATIC_SYSTEM_ENABLED")
endif()

if(ENABLE_DECALS)
    list(APPEND FEATURE_DEFINITIONS "DECAL_SYSTEM_ENABLED")
endif()

if(ENABLE_MESH_LOD)
    list(APPEND FEATURE_DEFINITIONS "MESH_LOD_ENABLED")
endif()

if(ENABLE_DXR)
    list(APPEND FEATURE_DEFINITIONS "DXR_ENABLED")
endif()

if(ENABLE_WEATHER)
    list(APPEND FEATURE_DEFINITIONS "WEATHER_SYSTEM_ENABLED")
endif()

if(ENABLE_INVENTORY)
    list(APPEND FEATURE_DEFINITIONS "INVENTORY_SYSTEM_ENABLED")
endif()

if(ENABLE_QUEST_SYSTEM)
    list(APPEND FEATURE_DEFINITIONS "QUEST_SYSTEM_ENABLED")
endif()

if(ENABLE_EVENT_SYSTEM)
    list(APPEND FEATURE_DEFINITIONS "EVENT_SYSTEM_ENABLED")
endif()

if(ENABLE_DAY_NIGHT)
    list(APPEND FEATURE_DEFINITIONS "DAY_NIGHT_CYCLE_ENABLED")
endif()

if(ENABLE_PERF_STATS)
    list(APPEND FEATURE_DEFINITIONS "PERF_STATS_ENABLED")
endif()

if(ENABLE_FOG_SYSTEM)
    list(APPEND FEATURE_DEFINITIONS "FOG_SYSTEM_ENABLED")
endif()

if(ENABLE_SCREEN_SPACE)
    list(APPEND FEATURE_DEFINITIONS "SCREEN_SPACE_EFFECTS_ENABLED")
endif()

if(ENABLE_SDL2)
    list(APPEND FEATURE_DEFINITIONS "SPARK_SDL2_AVAILABLE")
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        target_link_libraries(SparkEngineLib PUBLIC SDL2::SDL2)
        message(STATUS "SDL2 found and linked")
    else()
        message(STATUS "SDL2 not found - SDL2 input backend unavailable")
    endif()
endif()

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
message(STATUS "Systems:")
message(STATUS "  AI/NavMesh     : ${ENABLE_AI}")
message(STATUS "  Animation      : ${ENABLE_ANIMATION}")
message(STATUS "  Save System    : ${ENABLE_SAVE_SYSTEM}")
message(STATUS "  Procedural Gen : ${ENABLE_PROCEDURAL}")
message(STATUS "  Cinematic      : ${ENABLE_CINEMATIC}")
message(STATUS "  Decals         : ${ENABLE_DECALS}")
message(STATUS "  Mesh LOD       : ${ENABLE_MESH_LOD}")
message(STATUS "  DXR RayTracing : ${ENABLE_DXR}")
message(STATUS "  SDL2 Input     : ${ENABLE_SDL2}")
