cmake_minimum_required(VERSION 3.25)

project(SparkLauncher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Share the imgui target with SparkEditor (safe if already included).
include("${CMAKE_SOURCE_DIR}/cmake/BuildImGui.cmake")
if(NOT IMGUI_FOUND)
    message(WARNING "SparkLauncher: Dear ImGui not found — skipping build")
    return()
endif()

set(LAUNCHER_SOURCES
    src/main.cpp
    src/LauncherApp.cpp
    src/LauncherApp.h
    src/LauncherBackend.h
)
if(WIN32)
    list(APPEND LAUNCHER_SOURCES src/LauncherBackendWindows.cpp)
    add_executable(SparkLauncher WIN32 ${LAUNCHER_SOURCES})
    target_link_libraries(SparkLauncher PRIVATE d3d11 dxgi user32 gdi32 shell32 ole32 oleaut32 uuid comdlg32 advapi32)
else()
    list(APPEND LAUNCHER_SOURCES src/LauncherBackendLinux.cpp)
    add_executable(SparkLauncher ${LAUNCHER_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(SparkLauncher PRIVATE Threads::Threads)
endif()

# Pull ProjectManager + FolderDialog directly from the editor tree — no copy, no new lib.
target_sources(SparkLauncher PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/ProjectManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Utils/FolderDialog.cpp"
)

target_include_directories(SparkLauncher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
)

target_link_libraries(SparkLauncher PRIVATE imgui)
if(TARGET SparkEngineLib)
    target_link_libraries(SparkLauncher PRIVATE SparkEngineLib)
endif()

# Match the version macros SparkEditor defines so the shared ProjectManager.cpp compiles here.
target_compile_definitions(SparkLauncher PRIVATE
    EDITOR_VERSION_MAJOR=1
    EDITOR_VERSION_MINOR=0
    EDITOR_VERSION_PATCH=0
)

if(MSVC)
    target_compile_definitions(SparkLauncher PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS)
    set_property(TARGET SparkLauncher PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

install(TARGETS SparkLauncher RUNTIME DESTINATION bin COMPONENT runtime)
