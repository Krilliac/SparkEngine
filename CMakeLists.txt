cmake_minimum_required(VERSION 3.16)

# ================================================================
# Modern CMake Configuration - Cross-Platform Build
# ================================================================

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
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

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
        "Spark Engine/Source/Utils/TinyObjImpl.cpp"
    )
    target_include_directories(tinyobjloader PUBLIC ${TINYOBJLOADER_INCLUDE_DIR})
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
# 9) SparkEngine source files and executable
# ---------------------------------------------------------------------
file(GLOB_RECURSE SPARK_ENGINE_SOURCES
    CONFIGURE_DEPENDS
    "Spark Engine/Source/*.cpp"
    "Spark Engine/Source/*.h"
    "Spark Engine/Source/*.hpp"
)
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*[Tt]est.*")
list(FILTER SPARK_ENGINE_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# Create the main executable
# On Windows: WIN32 subsystem (uses wWinMain entry point)
# On Linux: Standard executable (uses main() entry point)
if(WIN32)
    add_executable(SparkEngine WIN32 ${SPARK_ENGINE_SOURCES})
else()
    add_executable(SparkEngine ${SPARK_ENGINE_SOURCES})
endif()

# Link third-party libraries properly
if(MINIZ_FOUND)
    target_link_libraries(SparkEngine PRIVATE miniz)
    message(STATUS "Linking SparkEngine with MINIZ")
endif()

if(TINYOBJ_FOUND)
    target_link_libraries(SparkEngine PRIVATE tinyobjloader)
    message(STATUS "Linking SparkEngine with TinyObjLoader")
endif()

# Link Bullet Physics
if(BULLET_FOUND)
    # Build Bullet from source via its own CMake
    set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/ThirdParty/Physics/bullet3" "${CMAKE_BINARY_DIR}/bullet3")
    target_link_libraries(SparkEngine PRIVATE BulletDynamics BulletCollision LinearMath)
    message(STATUS "Linking SparkEngine with Bullet Physics")
endif()

# Ensure SparkEngine depends on SparkConsole
if(TARGET SparkConsole)
    add_dependencies(SparkEngine SparkConsole)
    message(STATUS "SparkEngine will be built after SparkConsole")

    # FIXED: Simplified post-build step to copy SparkConsole.exe
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkConsole>
            $<TARGET_FILE_DIR:SparkEngine>
        COMMENT "Copying SparkConsole to SparkEngine output directory"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------
# 9.5) Build SparkEditor if enabled and exists
# ---------------------------------------------------------------------
if(ENABLE_EDITOR AND EXISTS "${CMAKE_SOURCE_DIR}/SparkEditor/CMakeLists.txt")
    message(STATUS "Adding SparkEditor to build...")
    add_subdirectory(SparkEditor)

    # Ensure SparkEditor is built after SparkEngine
    if(TARGET SparkEngine AND TARGET SparkEditor)
        add_dependencies(SparkEditor SparkEngine)
        message(STATUS "SparkEditor will be built after SparkEngine")
    endif()
endif()

# ---------------------------------------------------------------------
# 10) Asset Management
# ---------------------------------------------------------------------
# Create necessary directories
add_custom_command(TARGET SparkEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Models"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Textures"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Audio"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Scripts"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Config"
    COMMENT "Creating asset directory structure"
)

# Copy model assets to output directory
if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/Models")
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets/Models"
            "$<TARGET_FILE_DIR:SparkEngine>/Assets/Models"
        COMMENT "Copying model assets"
    )
endif()

# Copy scene files to output directory
if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/Scenes")
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets/Scenes"
            "$<TARGET_FILE_DIR:SparkEngine>/Assets/Scenes"
        COMMENT "Copying scene assets"
    )
endif()

# Copy shaders to output directory instead of compiling them
file(GLOB_RECURSE SHADER_FILES "Spark Engine/Shaders/HLSL/*.hlsl")
foreach(SHADER_FILE ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    add_custom_command(TARGET SparkEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADER_FILE}
            "$<TARGET_FILE_DIR:SparkEngine>/Shaders/${SHADER_NAME}"
        COMMENT "Copying shader ${SHADER_NAME}"
    )
endforeach()

# Copy GLSL shaders to output directory
if(EXISTS "${CMAKE_SOURCE_DIR}/Shaders/GLSL")
    file(GLOB GLSL_SHADER_FILES "${CMAKE_SOURCE_DIR}/Shaders/GLSL/*.glsl")
    foreach(SHADER_FILE ${GLSL_SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        add_custom_command(TARGET SparkEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Shaders/GLSL"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE}
                "$<TARGET_FILE_DIR:SparkEngine>/Shaders/GLSL/${SHADER_NAME}"
            COMMENT "Copying GLSL shader ${SHADER_NAME}"
        )
    endforeach()
endif()

# Copy SPIR-V compiled shaders to output directory
if(EXISTS "${CMAKE_SOURCE_DIR}/Shaders/SPIRV")
    file(GLOB SPIRV_SHADER_FILES "${CMAKE_SOURCE_DIR}/Shaders/SPIRV/*.spv")
    foreach(SHADER_FILE ${SPIRV_SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        add_custom_command(TARGET SparkEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEngine>/Shaders/SPIRV"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE}
                "$<TARGET_FILE_DIR:SparkEngine>/Shaders/SPIRV/${SHADER_NAME}"
            COMMENT "Copying SPIR-V shader ${SHADER_NAME}"
        )
    endforeach()
endif()

# Exclude shader files from automatic compilation (VS only)
if(MSVC)
    file(GLOB_RECURSE ALL_SHADER_FILES "*.hlsl")
    set_source_files_properties(${ALL_SHADER_FILES} PROPERTIES
        VS_TOOL_OVERRIDE "None"
        VS_DEPLOYMENT_CONTENT 1
        VS_DEPLOYMENT_LOCATION "Shaders"
    )
endif()

# ---------------------------------------------------------------------
# 11) Include directories
# ---------------------------------------------------------------------
target_include_directories(SparkEngine PRIVATE
    "Spark Engine/Source"
    ${THIRDPARTY_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------
# 12) System libraries - Platform-specific
# ---------------------------------------------------------------------
if(WIN32)
    target_link_libraries(SparkEngine PRIVATE
        # DirectX and graphics
        d3d11 dxgi d3dcompiler dxguid

        # Windows core libraries
        kernel32 user32 gdi32 winspool
        shell32 comdlg32 advapi32
        ole32 oleaut32 uuid

        # Multimedia and timing
        winmm

        # Networking libraries (only if networking is enabled)
        $<$<BOOL:${ENABLE_NETWORKING}>:ws2_32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wsock32>
        $<$<BOOL:${ENABLE_NETWORKING}>:crypt32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wldap32>
        $<$<BOOL:${ENABLE_NETWORKING}>:normaliz>
    )
else()
    # Linux/macOS system libraries
    find_package(Threads REQUIRED)
    target_link_libraries(SparkEngine PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}  # libdl for dynamic loading
    )
    # Platform define for cross-platform code
    target_compile_definitions(SparkEngine PRIVATE SPARK_PLATFORM_LINUX)
endif()

# Add legacy stdio for compatibility
if(MSVC)
    target_link_libraries(SparkEngine PRIVATE legacy_stdio_definitions)
endif()

# --- Vulkan backend libraries ---
if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkEngine PRIVATE SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkEngine PRIVATE Vulkan::Vulkan)
    else()
        target_include_directories(SparkEngine PRIVATE ${Vulkan_INCLUDE_DIR})
        target_link_libraries(SparkEngine PRIVATE ${Vulkan_LIBRARY})
    endif()
    message(STATUS "SparkEngine: Vulkan backend ENABLED")
endif()

# --- OpenGL backend libraries ---
if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkEngine PRIVATE SPARK_OPENGL_SUPPORT)
    target_link_libraries(SparkEngine PRIVATE OpenGL::GL)

    if(GLAD_FOUND)
        # Build GLAD as a static library
        add_library(glad STATIC "${GLAD_SOURCE_DIR}/glad.c")
        target_include_directories(glad PUBLIC "${GLAD_INCLUDE_DIR}")
        if(MSVC)
            set_property(TARGET glad PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        target_link_libraries(SparkEngine PRIVATE glad)
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
                target_link_libraries(SparkEngine PRIVATE sol2)
                target_compile_definitions(SparkEngine PRIVATE LUA_SCRIPTING_ENABLED)
                message(STATUS "Found Sol2 at: ${PATH}")
            else()
                target_include_directories(SparkEngine PRIVATE "${CMAKE_SOURCE_DIR}/${PATH}")
                message(STATUS "Found Lua headers at: ${PATH}")
            endif()
        endif()
    endforeach()
endif()

if(ENABLE_PHYSX)
    if(EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/Physics/PhysX/include")
        target_include_directories(SparkEngine PRIVATE "ThirdParty/Physics/PhysX/include")
        target_compile_definitions(SparkEngine PRIVATE PHYSX_AVAILABLE)
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
        target_link_libraries(SparkEngine PRIVATE SDL2::SDL2)
        message(STATUS "SDL2 found and linked")
    else()
        message(STATUS "SDL2 not found - SDL2 input backend unavailable")
    endif()
endif()

# Apply feature definitions to target
if(FEATURE_DEFINITIONS)
    target_compile_definitions(SparkEngine PRIVATE ${FEATURE_DEFINITIONS})
endif()

# ---------------------------------------------------------------------
# 15) Visual Studio settings
# ---------------------------------------------------------------------
if(MSVC)
    foreach(src ${SPARK_ENGINE_SOURCES})
        get_filename_component(dir "${src}" DIRECTORY)
        file(RELATIVE_PATH grp "${CMAKE_SOURCE_DIR}/Spark Engine/Source" "${dir}")
        string(REPLACE "/" "\\\\" grp "${grp}")
        if(grp STREQUAL "")
            source_group("Source Files" FILES "${src}")
        else()
            source_group("Source Files\\\\${grp}" FILES "${src}")
        endif()
    endforeach()

    # Set working directory and startup project
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
# 17) Build summary
# ---------------------------------------------------------------------
message(STATUS "=== SPARKENGINE CONFIGURED SUCCESSFULLY ===")
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
