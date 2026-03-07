cmake_minimum_required(VERSION 3.16)

# ================================================================
# SparkGame - Game Module DLL
#
# SparkGame compiles as a shared library (DLL on Windows, .so on Linux)
# that the SparkEngine executable loads at runtime via GameModuleLoader.
#
# This CMakeLists.txt can be used in two ways:
#   1. As a subdirectory of the main SparkEngine build (typical dev workflow)
#   2. Standalone: generates its own solution files independently
#      cmake -B build -DSPARK_ENGINE_DIR=<path-to-engine>
#
# Similar to how Unreal Engine game projects generate their own solution
# files and compile game code into modules that the engine loads.
# ================================================================

# When built standalone, this is the top-level project
if(NOT CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL "SparkGame")
    project(SparkGame LANGUAGES CXX)
    set(SPARK_GAME_STANDALONE TRUE)
else()
    set(SPARK_GAME_STANDALONE FALSE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------
# Standalone mode: locate SparkEngine
# ---------------------------------------------------------------------
if(SPARK_GAME_STANDALONE)
    # User must provide the engine path, or we assume it's the parent directory
    if(NOT SPARK_ENGINE_DIR)
        set(SPARK_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
            "Path to SparkEngine root directory")
    endif()

    message(STATUS "SparkGame standalone build - Engine at: ${SPARK_ENGINE_DIR}")

    # Output directories
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
    endforeach()

    # Compiler settings for standalone
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_compile_options(/W3 /MP /bigobj /wd4005 /wd4996 /wd4244 /wd4267)
        add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Wno-unused-parameter -fPIC)
    endif()

    # Third-party include directories
    set(THIRDPARTY_INCLUDE_DIRS "")
    if(EXISTS "${SPARK_ENGINE_DIR}/ThirdParty/ECS/entt/single_include")
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ENGINE_DIR}/ThirdParty/ECS/entt/single_include")
    endif()
    if(EXISTS "${SPARK_ENGINE_DIR}/ThirdParty/Physics/bullet3/src")
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ENGINE_DIR}/ThirdParty/Physics/bullet3/src")
    endif()
    if(EXISTS "${SPARK_ENGINE_DIR}/ThirdParty/UI/imgui")
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ENGINE_DIR}/ThirdParty/UI/imgui")
    endif()

    set(ENGINE_SOURCE_DIR "${SPARK_ENGINE_DIR}/SparkEngine/Source")
else()
    # Sub-project mode: engine source dir is relative to root
    set(ENGINE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/SparkEngine/Source")
endif()

# ---------------------------------------------------------------------
# Collect SparkGame source files
# ---------------------------------------------------------------------
file(GLOB_RECURSE SPARK_GAME_SOURCES
    CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.h"
    "Source/*.hpp"
)
list(FILTER SPARK_GAME_SOURCES EXCLUDE REGEX ".*[Tt]est.*")
list(FILTER SPARK_GAME_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# ---------------------------------------------------------------------
# Create the game as a SHARED library (DLL)
# ---------------------------------------------------------------------
add_library(SparkGame SHARED ${SPARK_GAME_SOURCES})

# Mark this as a game DLL so SPARK_GAME_API and SPARK_MODULE_API export symbols
target_compile_definitions(SparkGame PRIVATE SPARK_GAME_DLL SPARK_MODULE_DLL)

# ---------------------------------------------------------------------
# Link against SparkEngineLib static library (when built as sub-project)
# SparkEngineLib contains all engine systems; SparkEngine is the exe host.
# In standalone mode, the game must provide its own engine lib path.
# ---------------------------------------------------------------------
if(NOT SPARK_GAME_STANDALONE AND TARGET SparkEngineLib)
    target_link_libraries(SparkGame PRIVATE SparkEngineLib)
endif()

# ---------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------
# SparkSDK includes (new module system headers)
if(SPARK_GAME_STANDALONE)
    set(SPARK_SDK_INCLUDE_DIR "${SPARK_ENGINE_DIR}/SparkSDK/Include")
else()
    set(SPARK_SDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
endif()

target_include_directories(SparkGame PRIVATE
    "Source"
    "${ENGINE_SOURCE_DIR}"
    "${SPARK_SDK_INCLUDE_DIR}"
    ${THIRDPARTY_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------
# Platform-specific libraries
# ---------------------------------------------------------------------
if(WIN32)
    target_link_libraries(SparkGame PRIVATE
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

    target_compile_definitions(SparkGame PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(SparkGame PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(SparkGame PRIVATE SPARK_PLATFORM_LINUX)
endif()

if(MSVC)
    target_link_libraries(SparkGame PRIVATE legacy_stdio_definitions)
endif()

# Link Bullet Physics if available
if(TARGET BulletDynamics)
    target_link_libraries(SparkGame PRIVATE BulletDynamics BulletCollision LinearMath)
endif()

# Link third-party libraries
if(TARGET miniz)
    target_link_libraries(SparkGame PRIVATE miniz)
endif()
if(TARGET tinyobjloader)
    target_link_libraries(SparkGame PRIVATE tinyobjloader)
endif()

# Vulkan backend
if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkGame PRIVATE SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkGame PRIVATE Vulkan::Vulkan)
    else()
        target_include_directories(SparkGame PRIVATE ${Vulkan_INCLUDE_DIR})
        target_link_libraries(SparkGame PRIVATE ${Vulkan_LIBRARY})
    endif()
endif()

# OpenGL backend
if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkGame PRIVATE SPARK_OPENGL_SUPPORT)
    target_link_libraries(SparkGame PRIVATE OpenGL::GL)
    if(TARGET glad)
        target_link_libraries(SparkGame PRIVATE glad)
    endif()
endif()

# Apply feature definitions (inherited from root CMakeLists.txt via FEATURE_DEFINITIONS)
if(FEATURE_DEFINITIONS)
    target_compile_definitions(SparkGame PRIVATE ${FEATURE_DEFINITIONS})
endif()

# ---------------------------------------------------------------------
# Post-build: Copy the game DLL next to the engine executable
# ---------------------------------------------------------------------
if(NOT SPARK_GAME_STANDALONE)
    if(TARGET SparkEngine)
        add_custom_command(TARGET SparkGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SparkGame>
                $<TARGET_FILE_DIR:SparkEngine>
            COMMENT "Copying SparkGame DLL to engine output directory"
            VERBATIM
        )
    endif()
endif()

# ---------------------------------------------------------------------
# Asset Management
# ---------------------------------------------------------------------
add_custom_command(TARGET SparkGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Assets"
    COMMENT "Creating asset directory structure"
)

# Copy model assets
set(ASSET_ROOT "${CMAKE_SOURCE_DIR}")
if(SPARK_GAME_STANDALONE)
    set(ASSET_ROOT "${SPARK_ENGINE_DIR}")
endif()

if(EXISTS "${ASSET_ROOT}/Assets/Models")
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ASSET_ROOT}/Assets/Models"
            "$<TARGET_FILE_DIR:SparkGame>/Assets/Models"
        COMMENT "Copying model assets"
    )
endif()

if(EXISTS "${ASSET_ROOT}/Assets/Scenes")
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ASSET_ROOT}/Assets/Scenes"
            "$<TARGET_FILE_DIR:SparkGame>/Assets/Scenes"
        COMMENT "Copying scene assets"
    )
endif()

# Copy shaders from engine
set(SHADER_SOURCE_DIR "${ENGINE_SOURCE_DIR}/../Shaders/HLSL")
if(NOT EXISTS "${SHADER_SOURCE_DIR}")
    set(SHADER_SOURCE_DIR "${ASSET_ROOT}/SparkEngine/Shaders/HLSL")
endif()
if(EXISTS "${SHADER_SOURCE_DIR}")
    file(GLOB_RECURSE SHADER_FILES "${SHADER_SOURCE_DIR}/*.hlsl")
    foreach(SHADER_FILE ${SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        add_custom_command(TARGET SparkGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE}
                "$<TARGET_FILE_DIR:SparkGame>/Shaders/${SHADER_NAME}"
            COMMENT "Copying shader ${SHADER_NAME}"
        )
    endforeach()
endif()

# Ensure SparkConsole is copied if available
if(TARGET SparkConsole)
    add_dependencies(SparkGame SparkConsole)
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkConsole>
            $<TARGET_FILE_DIR:SparkGame>
        COMMENT "Copying SparkConsole to output directory"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------
# Visual Studio settings
# ---------------------------------------------------------------------
if(MSVC)
    foreach(src ${SPARK_GAME_SOURCES})
        get_filename_component(dir "${src}" DIRECTORY)
        file(RELATIVE_PATH grp "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${dir}")
        string(REPLACE "/" "\\\\" grp "${grp}")
        if(grp STREQUAL "")
            source_group("Source Files" FILES "${src}")
        else()
            source_group("Source Files\\\\${grp}" FILES "${src}")
        endif()
    endforeach()

    if(SPARK_GAME_STANDALONE)
        set_property(TARGET SparkGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${SPARK_ENGINE_DIR}")
    else()
        set_property(TARGET SparkGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()

    # Exclude shader files from compilation
    file(GLOB_RECURSE ALL_SHADER_FILES "*.hlsl")
    if(ALL_SHADER_FILES)
        set_source_files_properties(${ALL_SHADER_FILES} PROPERTIES
            VS_TOOL_OVERRIDE "None"
        )
    endif()
endif()

message(STATUS "SparkGame configured as SHARED LIBRARY (game module DLL)")
if(SPARK_GAME_STANDALONE)
    message(STATUS "  Mode: Standalone build")
    message(STATUS "  Engine: ${SPARK_ENGINE_DIR}")
else()
    message(STATUS "  Mode: Sub-project of SparkEngine")
endif()
