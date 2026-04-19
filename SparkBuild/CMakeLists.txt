cmake_minimum_required(VERSION 3.16)
project(SparkBuild LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SparkBuild - Cross-platform CMake Build Tool for SparkEngine
# Supports: Windows, Linux, macOS
#
# Split layout:
#   SparkBuildCore  - reusable static library (Config, Downloader, ProcessRunner, Terminal)
#                     linked by both the SparkBuild executable and SparkInstaller
#   SparkBuild      - interactive TUI executable (main.cpp + SparkBuild.cpp on top of core)

# ---------------------------------------------------------------------------
# SparkBuildCore - reusable static library
# ---------------------------------------------------------------------------
set(SPARKBUILDCORE_SOURCES
    src/Platform.h
    src/Config.h
    src/Config.cpp
    src/ProcessRunner.h
    src/ProcessRunner.cpp
    src/Downloader.h
    src/Downloader.cpp
    src/Terminal.h
    src/Terminal.cpp
)

add_library(SparkBuildCore STATIC ${SPARKBUILDCORE_SOURCES})

target_include_directories(SparkBuildCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(SparkBuildCore PUBLIC
        winhttp       # HTTP downloads (WinHTTP API)
        shlwapi       # Shell lightweight utility functions
        shell32       # Shell API (folder dialogs, ZIP extraction)
        ole32         # COM runtime
        oleaut32      # OLE Automation
        uuid          # COM GUIDs
    )
    target_compile_definitions(SparkBuildCore PUBLIC
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

if(MSVC)
    set_property(TARGET SparkBuildCore PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(SparkBuildCore PUBLIC pthread)
    target_compile_options(SparkBuildCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# SparkBuild - interactive TUI executable
# ---------------------------------------------------------------------------
set(SPARKBUILD_SOURCES
    src/main.cpp
    src/SparkBuild.h
    src/SparkBuild.cpp
)

add_executable(SparkBuild ${SPARKBUILD_SOURCES})

if(WIN32)
    target_sources(SparkBuild PRIVATE resources/SparkBuild.rc)
endif()

target_link_libraries(SparkBuild PRIVATE SparkBuildCore)

if(MSVC)
    set_property(TARGET SparkBuild PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
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
