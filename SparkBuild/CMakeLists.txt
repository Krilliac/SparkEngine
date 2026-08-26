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

# Inherit the top-level /MD[d] CRT (CMAKE_MSVC_RUNTIME_LIBRARY is set by the
# engine's root CMakeLists). Forcing /MT[d] here causes SparkInstaller — which
# also links imgui (built with /MD[d] via BuildImGui.cmake) — to pull in both
# CRT variants and fail with LNK1169 / LNK2005 on duplicate symbols.

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

# Inherit the top-level /MD[d] CRT — see comment above SparkBuildCore.

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SparkBuild PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(SPARKBUILD_BUILD_TESTS "Build SparkBuild focused regression tests" ON)
if(SPARKBUILD_BUILD_TESTS)
    enable_testing()
    add_executable(SparkBuildProcessRunnerTests
        tests/ProcessRunnerTests.cpp
    )
    target_link_libraries(SparkBuildProcessRunnerTests PRIVATE SparkBuildCore)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(SparkBuildProcessRunnerTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME SparkBuildProcessRunnerTests COMMAND SparkBuildProcessRunnerTests)
    set_tests_properties(SparkBuildProcessRunnerTests PROPERTIES TIMEOUT 25)

    add_executable(SparkBuildDownloaderTests
        tests/DownloaderTests.cpp
    )
    target_link_libraries(SparkBuildDownloaderTests PRIVATE SparkBuildCore)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(SparkBuildDownloaderTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME SparkBuildDownloaderTests COMMAND SparkBuildDownloaderTests)
    set_tests_properties(SparkBuildDownloaderTests PROPERTIES TIMEOUT 10)
endif()

# Standalone consumers retain the historical flat output. The SparkEngine
# superproject supplies configuration-specific directories for multi-config
# generators so Debug and Release artifacts cannot overwrite one another.
if(NOT CMAKE_PROJECT_NAME STREQUAL "SparkEngine")
    set_target_properties(SparkBuild PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# Install alongside other SparkEngine tools so CPack picks it up.
install(TARGETS SparkBuild RUNTIME DESTINATION bin COMPONENT tools)
