cmake_minimum_required(VERSION 3.16)

project(SparkEditor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find ImGui
find_path(IMGUI_INCLUDE_DIR 
    NAMES imgui.h
    PATHS 
        "${CMAKE_SOURCE_DIR}/ThirdParty/imgui"
        "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui"
        "${CMAKE_SOURCE_DIR}/External/imgui"
        "${CMAKE_SOURCE_DIR}/Vendor/imgui"
    DOC "Dear ImGui include directory"
)

if(IMGUI_INCLUDE_DIR)
    message(STATUS "Found Dear ImGui at: ${IMGUI_INCLUDE_DIR}")

    # Only create the imgui target if it doesn't already exist (may be defined in ThirdParty)
    if(NOT TARGET imgui)
        add_library(imgui STATIC
            "${IMGUI_INCLUDE_DIR}/imgui.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_demo.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_draw.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_tables.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_widgets.cpp"
            "${IMGUI_INCLUDE_DIR}/backends/imgui_impl_win32.cpp"
            "${IMGUI_INCLUDE_DIR}/backends/imgui_impl_dx11.cpp"
        )

        target_include_directories(imgui PUBLIC
            "${IMGUI_INCLUDE_DIR}"
            "${IMGUI_INCLUDE_DIR}/backends"
        )

        target_compile_definitions(imgui PUBLIC
            IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
            IMGUI_IMPL_WIN32_DISABLE_LINKING_XINPUT
        )

        target_link_libraries(imgui PUBLIC
            d3d11 dxgi user32 gdi32 shell32 ole32 oleaut32 uuid comdlg32 advapi32
        )

        if(MSVC)
            target_compile_options(imgui PRIVATE /W2)
            target_compile_definitions(imgui PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                _CRT_SECURE_NO_WARNINGS
            )
            set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endif()

    set(IMGUI_FOUND TRUE)
else()
    message(FATAL_ERROR "Dear ImGui is required for SparkEditor but was not found!")
    set(IMGUI_FOUND FALSE)
endif()

# Collect source files
file(GLOB_RECURSE SPARK_EDITOR_SOURCES
    CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.h"
    "Source/*.hpp"
)

# Remove test and variant files
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*[Tt]est.*")
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*main_debug\\.cpp$")
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*_Fixed\\.cpp$")
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*_Enhanced\\.cpp$") 
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*_Updated\\.cpp$")
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*_EngineIntegrated\\.cpp$")
list(FILTER SPARK_EDITOR_SOURCES EXCLUDE REGEX ".*_AutoConnect\\.(cpp|h)$")

# Create executable
add_executable(SparkEditor WIN32 ${SPARK_EDITOR_SOURCES})

# Include directories
target_include_directories(SparkEditor PRIVATE
    "Source"
    "${CMAKE_SOURCE_DIR}/Shared"
    "${CMAKE_SOURCE_DIR}/Spark Engine/Source"
)

if(IMGUI_FOUND)
    target_include_directories(SparkEditor PRIVATE
        "${IMGUI_INCLUDE_DIR}"
        "${IMGUI_INCLUDE_DIR}/backends"
    )
endif()

# Preprocessor definitions
target_compile_definitions(SparkEditor PRIVATE
    SPARK_EDITOR_BUILD
    EDITOR_VERSION_MAJOR=1
    EDITOR_VERSION_MINOR=0
    EDITOR_VERSION_PATCH=0
    GRAPHICS_API_DX11
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    ENGINE_INTEGRATION_ENABLED
)

target_compile_definitions(SparkEditor PRIVATE
    $<$<CONFIG:Debug>:
        SPARK_DEBUG
        ENABLE_ASSERTIONS
        ENABLE_LOGGING
        ENABLE_PROFILING
        IMGUI_DEBUG
    >
)

target_compile_definitions(SparkEditor PRIVATE
    $<$<CONFIG:Release>:
        SPARK_RELEASE
        NDEBUG
    >
)

# Compiler options
if(MSVC)
    target_compile_options(SparkEditor PRIVATE
        /W3 /MP /bigobj /std:c++20
        /wd4005 /wd4996 /wd4244 /wd4267 /wd4251 /wd4275
        $<$<CONFIG:Release>:/O2 /Oi /Ot /Oy /GL>
        $<$<CONFIG:Debug>:/Od /RTC1>
    )
    
    target_link_options(SparkEditor PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
    
    target_link_libraries(SparkEditor PRIVATE legacy_stdio_definitions)
endif()

# Link libraries
if(IMGUI_FOUND)
    target_link_libraries(SparkEditor PRIVATE imgui)
endif()

target_link_libraries(SparkEditor PRIVATE
    d3d11 dxgi d3dcompiler dxguid
    kernel32 user32 gdi32 winspool comdlg32 advapi32 shell32 ole32 oleaut32 uuid
    odbc32 odbccp32 winmm shlwapi comctl32 version
)

if(TARGET miniz)
    target_link_libraries(SparkEditor PRIVATE miniz)
endif()

# Post-build steps
add_custom_command(TARGET SparkEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEditor>/EditorAssets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEditor>/EditorAssets/Icons"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEditor>/EditorAssets/Themes"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEditor>/EditorAssets/Layouts"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEditor>/Projects"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkEditor>/Temp"
    COMMENT "Creating SparkEditor directory structure"
)

# Dependencies
if(TARGET SparkEngine)
    add_dependencies(SparkEditor SparkEngine)
    add_custom_command(TARGET SparkEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkEngine>
            $<TARGET_FILE_DIR:SparkEditor>/$<TARGET_FILE_NAME:SparkEngine>
        COMMENT "Installing SparkEngine for editor integration"
    )
endif()

if(TARGET SparkConsole)
    add_custom_command(TARGET SparkEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkConsole>
            $<TARGET_FILE_DIR:SparkEditor>/$<TARGET_FILE_NAME:SparkConsole>
        COMMENT "Installing SparkConsole for editor debugging"
    )
endif()

# Visual Studio settings
if(MSVC)
    set_property(TARGET SparkEditor PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    
    foreach(source ${SPARK_EDITOR_SOURCES})
        get_filename_component(source_path "${source}" DIRECTORY)
        file(RELATIVE_PATH source_relative "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${source_path}")
        string(REPLACE "/" "\\\\" group_path "${source_relative}")
        
        if(group_path STREQUAL "")
            source_group("Source Files" FILES "${source}")
        else()
            source_group("Source Files\\\\${group_path}" FILES "${source}")
        endif()
    endforeach()
    
    source_group("Core Systems" REGULAR_EXPRESSION "Source/Core/.*")
    source_group("UI Panels" REGULAR_EXPRESSION "Source/Panels/.*")
    source_group("Scene Management" REGULAR_EXPRESSION "Source/SceneSystem/.*")
    source_group("Asset Management" REGULAR_EXPRESSION "Source/AssetBrowser/.*")
    source_group("Communication" REGULAR_EXPRESSION "Source/Communication/.*")
    source_group("Viewport" REGULAR_EXPRESSION "Source/Viewport/.*")
    
    set_property(TARGET SparkEditor PROPERTY VS_GLOBAL_KEYWORD "Win32Proj")
    set_property(TARGET SparkEditor PROPERTY VS_GLOBAL_ROOTNAMESPACE "SparkEditor")
    set_property(TARGET SparkEditor PROPERTY VS_GLOBAL_WindowsTargetPlatformVersion "10.0")
endif()

message(STATUS "SparkEditor CMake configuration completed successfully!")
message(STATUS "Debug target removed - single SparkEditor target only")
message(STATUS "Main_debug.cpp merged into main.cpp")