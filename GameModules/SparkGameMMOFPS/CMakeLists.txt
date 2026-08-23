cmake_minimum_required(VERSION 3.25)

# ================================================================
# SparkGameMMOFPS - TERRAFRONT MMOFPS Game Module DLL
#
# Showcases SparkEngine's MMO networking stack:
#   - WorldServer / AreaServer architecture
#   - Seamless area streaming
#   - Entity replication with delta compression
#   - Client-side prediction and server reconciliation
#   - World origin rebasing for large worlds
#   - Chat with area/global/party channels
#
# Built as a shared library loaded by the engine at runtime,
# identical to SparkGame's module pattern.
# ================================================================

if(NOT CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL "SparkGameMMOFPS")
    project(SparkGameMMOFPS LANGUAGES CXX)
    set(SPARK_GAME_MMOFPS_STANDALONE TRUE)
else()
    set(SPARK_GAME_MMOFPS_STANDALONE FALSE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------
# Standalone mode: locate SparkEngine
# ---------------------------------------------------------------------
if(SPARK_GAME_MMOFPS_STANDALONE)
    if(NOT SPARK_ENGINE_DIR)
        set(SPARK_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
            "Path to SparkEngine root directory")
    endif()

    message(STATUS "SparkGameMMOFPS standalone build - Engine at: ${SPARK_ENGINE_DIR}")

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
    endforeach()

    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_compile_options(/W3 /MP /bigobj /wd4005 /wd4996 /wd4244 /wd4267)
        add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Wno-unused-parameter -fPIC)
    endif()

    set(THIRDPARTY_INCLUDE_DIRS "")
    if(EXISTS "${SPARK_ENGINE_DIR}/ThirdParty/ECS/entt/single_include")
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ENGINE_DIR}/ThirdParty/ECS/entt/single_include")
    endif()
    if(EXISTS "${SPARK_ENGINE_DIR}/ThirdParty/UI/imgui")
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ENGINE_DIR}/ThirdParty/UI/imgui")
    endif()

    set(ENGINE_SOURCE_DIR "${SPARK_ENGINE_DIR}/SparkEngine/Source")
else()
    set(ENGINE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/SparkEngine/Source")
endif()

# ---------------------------------------------------------------------
# Collect SparkGameMMOFPS source files
# ---------------------------------------------------------------------
file(GLOB_RECURSE SPARK_GAME_MMOFPS_SOURCES
    CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.h"
    "Source/*.hpp"
)
list(FILTER SPARK_GAME_MMOFPS_SOURCES EXCLUDE REGEX ".*[Tt]est.*")
list(FILTER SPARK_GAME_MMOFPS_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# SceneManager's [Object] primitives (CubeObject & co.) are declared in the
# engine include path but defined in SparkGameFPS sources; SparkEngineLib does
# not compile them, so any module that pulls SceneManager.obj must build them
# itself (SparkGameFPS does the same implicitly via its glob).
set(_TF_FPS_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SparkGameFPS/Source/Game")
list(APPEND SPARK_GAME_MMOFPS_SOURCES
    "${_TF_FPS_GAME_DIR}/CubeObject.cpp"
    "${_TF_FPS_GAME_DIR}/PlaneObject.cpp"
    "${_TF_FPS_GAME_DIR}/SphereObject.cpp"
    "${_TF_FPS_GAME_DIR}/PyramidObject.cpp"
    "${_TF_FPS_GAME_DIR}/RampObject.cpp"
    "${_TF_FPS_GAME_DIR}/WallObject.cpp"
)

# ---------------------------------------------------------------------
# Create the game as a SHARED library (DLL)
# ---------------------------------------------------------------------
add_library(SparkGameMMOFPS SHARED ${SPARK_GAME_MMOFPS_SOURCES})

if(NOT COMMAND spark_configure_module_abi)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/SparkGameModule.cmake")
endif()
spark_configure_module_abi(SparkGameMMOFPS)

target_compile_definitions(SparkGameMMOFPS PRIVATE SPARK_GAME_DLL SPARK_MODULE_DLL)

# ---------------------------------------------------------------------
# Link against SparkEngineLib
# ---------------------------------------------------------------------
if(NOT SPARK_GAME_MMOFPS_STANDALONE AND TARGET SparkEngineLib)
    if(WIN32)
        target_link_libraries(SparkGameMMOFPS PRIVATE SparkEngineLib)
    elseif(TARGET SparkEngineInterface)
        # Headers + defines only; symbols resolve from exe via ENABLE_EXPORTS
        target_link_libraries(SparkGameMMOFPS PRIVATE SparkEngineInterface)
    endif()
endif()

# ---------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------
if(SPARK_GAME_MMOFPS_STANDALONE)
    set(SPARK_SDK_INCLUDE_DIR "${SPARK_ENGINE_DIR}/SparkSDK/Include")
else()
    set(SPARK_SDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
endif()

target_include_directories(SparkGameMMOFPS PRIVATE
    "Source"
    "${ENGINE_SOURCE_DIR}"
    "${SPARK_SDK_INCLUDE_DIR}"
    ${THIRDPARTY_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------
# Platform-specific libraries
# ---------------------------------------------------------------------
if(WIN32)
    target_link_libraries(SparkGameMMOFPS PRIVATE
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
    target_compile_definitions(SparkGameMMOFPS PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(SparkGameMMOFPS PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    if(APPLE)
        target_compile_definitions(SparkGameMMOFPS PRIVATE SPARK_PLATFORM_MACOS)
    else()
        target_compile_definitions(SparkGameMMOFPS PRIVATE SPARK_PLATFORM_LINUX)
    endif()
endif()

if(MSVC)
    target_link_libraries(SparkGameMMOFPS PRIVATE legacy_stdio_definitions)
endif()

# ---------------------------------------------------------------------
# Game-mode ImGui HUD (Windows): the DLL statically links its own copy of
# the ImGui object code; the engine injects its context + allocators at
# module load via the SparkModuleInjectImGui export (ModuleDllMain.h).
# SPARK_HAS_IMGUI is the truthful guard for the module's UI code — the HUD
# works in plain game mode, no ENABLE_EDITOR involved.
# ---------------------------------------------------------------------
if(WIN32)
    if(SPARK_GAME_MMOFPS_STANDALONE)
        include("${SPARK_ENGINE_DIR}/cmake/BuildImGui.cmake")
    else()
        include("${CMAKE_SOURCE_DIR}/cmake/BuildImGui.cmake")
    endif()
    if(TARGET imgui)
        target_link_libraries(SparkGameMMOFPS PRIVATE imgui)
        target_compile_definitions(SparkGameMMOFPS PRIVATE SPARK_HAS_IMGUI=1)
        message(STATUS "SparkGameMMOFPS: ImGui HUD ENABLED (game mode)")
    endif()
endif()

# Link Jolt Physics if available
# On Linux, static third-party libs resolve from the exe (ENABLE_EXPORTS)
if(WIN32)
    if(TARGET Jolt)
        target_link_libraries(SparkGameMMOFPS PRIVATE Jolt)
    endif()

    if(TARGET miniz)
        target_link_libraries(SparkGameMMOFPS PRIVATE miniz)
    endif()
    if(TARGET tinyobjloader)
        target_link_libraries(SparkGameMMOFPS PRIVATE tinyobjloader)
    endif()
endif()

# Vulkan backend
if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkGameMMOFPS PRIVATE SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkGameMMOFPS PRIVATE Vulkan::Vulkan)
    else()
        target_include_directories(SparkGameMMOFPS PRIVATE ${Vulkan_INCLUDE_DIR})
        target_link_libraries(SparkGameMMOFPS PRIVATE ${Vulkan_LIBRARY})
    endif()
endif()

# OpenGL backend
if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkGameMMOFPS PRIVATE SPARK_OPENGL_SUPPORT)
    target_link_libraries(SparkGameMMOFPS PRIVATE OpenGL::GL)
    if(TARGET glad)
        target_link_libraries(SparkGameMMOFPS PRIVATE glad)
    endif()
endif()

# Apply feature definitions
if(FEATURE_DEFINITIONS)
    target_compile_definitions(SparkGameMMOFPS PRIVATE ${FEATURE_DEFINITIONS})
endif()

# ---------------------------------------------------------------------
# Post-build: Asset directories
# ---------------------------------------------------------------------
add_custom_command(TARGET SparkGameMMOFPS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGameMMOFPS>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGameMMOFPS>/Assets"
    COMMENT "Creating SparkGameMMOFPS asset directory structure"
)

# Copy shaders from engine
set(SHADER_SOURCE_DIR "${ENGINE_SOURCE_DIR}/../Shaders/HLSL")
if(NOT EXISTS "${SHADER_SOURCE_DIR}")
    set(SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/SparkEngine/Shaders/HLSL")
    if(SPARK_GAME_MMOFPS_STANDALONE)
        set(SHADER_SOURCE_DIR "${SPARK_ENGINE_DIR}/SparkEngine/Shaders/HLSL")
    endif()
endif()
if(EXISTS "${SHADER_SOURCE_DIR}")
    file(GLOB_RECURSE SHADER_FILES "${SHADER_SOURCE_DIR}/*.hlsl")
    foreach(SHADER_FILE ${SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        add_custom_command(TARGET SparkGameMMOFPS POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE}
                "$<TARGET_FILE_DIR:SparkGameMMOFPS>/Shaders/${SHADER_NAME}"
            COMMENT "Copying shader ${SHADER_NAME}"
        )
    endforeach()
endif()

# ---------------------------------------------------------------------
# Visual Studio settings
# ---------------------------------------------------------------------
if(MSVC)
    foreach(src ${SPARK_GAME_MMOFPS_SOURCES})
        get_filename_component(dir "${src}" DIRECTORY)
        file(RELATIVE_PATH grp "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${dir}")
        string(REPLACE "/" "\\\\" grp "${grp}")
        if(grp STREQUAL "")
            source_group("Source Files" FILES "${src}")
        else()
            source_group("Source Files\\\\${grp}" FILES "${src}")
        endif()
    endforeach()

    if(SPARK_GAME_MMOFPS_STANDALONE)
        set_property(TARGET SparkGameMMOFPS PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${SPARK_ENGINE_DIR}")
    else()
        set_property(TARGET SparkGameMMOFPS PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endif()

message(STATUS "SparkGameMMOFPS configured as SHARED LIBRARY (TERRAFRONT MMOFPS game module DLL)")
if(SPARK_GAME_MMOFPS_STANDALONE)
    message(STATUS "  Mode: Standalone build")
    message(STATUS "  Engine: ${SPARK_ENGINE_DIR}")
else()
    message(STATUS "  Mode: Sub-project of SparkEngine")
endif()
