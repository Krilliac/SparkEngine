cmake_minimum_required(VERSION 3.16)
project(SparkBuild LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SparkBuild - Cross-platform CMake Build Tool for SparkEngine
# Supports: Windows, Linux, macOS

set(SPARKBUILD_SOURCES
    src/main.cpp
    src/Platform.h
    src/Config.h
    src/Config.cpp
    src/ProcessRunner.h
    src/ProcessRunner.cpp
    src/Downloader.h
    src/Downloader.cpp
    src/Terminal.h
    src/Terminal.cpp
    src/SparkBuild.h
    src/SparkBuild.cpp
)

# Console application (cross-platform TUI)
add_executable(SparkBuild ${SPARKBUILD_SOURCES})

# Resource file for Windows (version info, icon)
if(WIN32)
    target_sources(SparkBuild PRIVATE resources/SparkBuild.rc)
endif()

target_include_directories(SparkBuild PRIVATE src)

# Platform-specific linking
if(WIN32)
    target_link_libraries(SparkBuild PRIVATE
        winhttp       # HTTP downloads (WinHTTP API)
        shlwapi       # Shell lightweight utility functions
        shell32       # Shell API (folder dialogs, ZIP extraction)
        ole32         # COM runtime
        oleaut32      # OLE Automation
        uuid          # COM GUIDs
    )

    target_compile_definitions(SparkBuild PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

# MSVC-specific settings
if(MSVC)
    set_property(TARGET SparkBuild PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# GCC/Clang settings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(SparkBuild PRIVATE pthread)
    target_compile_options(SparkBuild PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Output to a predictable location
set_target_properties(SparkBuild PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# Install alongside other SparkEngine tools so CPack picks it up.
install(TARGETS SparkBuild RUNTIME DESTINATION bin)
