cmake_minimum_required(VERSION 3.16)
project(SparkInstaller VERSION 1.0.0 LANGUAGES CXX)

# SparkInstaller — bootstrap installer/updater for SparkEngine.
# Depends only on SparkBuildCore (no engine headers, no runtime linkage).
#
# Options:
#   SPARKINSTALLER_ENABLE_GUI  Build ImGui wizard frontend (default: ON when ImGui is available)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SPARKINSTALLER_ENABLE_GUI "Build SparkInstaller GUI wizard (requires ImGui)" ON)

if(NOT TARGET SparkBuildCore)
    message(FATAL_ERROR "SparkInstaller requires the SparkBuildCore target. "
                        "Ensure SparkBuild is configured before SparkInstaller.")
endif()

set(INSTALLER_SOURCES
    src/main.cpp
    src/Installer.h
    src/Installer.cpp
    src/InstallerContext.h
    src/GitRunner.h
    src/GitRunner.cpp
    src/GitBootstrap.h
    src/GitBootstrap.cpp
    src/InstallState.h
    src/InstallState.cpp
    src/tui/WizardTui.h
    src/tui/WizardTui.cpp
)

# ---------------------------------------------------------------------------
# Optional GUI frontend (ImGui + platform backend)
# ---------------------------------------------------------------------------
set(INSTALLER_GUI_AVAILABLE OFF)
if(SPARKINSTALLER_ENABLE_GUI)
    # If the engine superproject already built ImGui via BuildImGui.cmake, reuse that target.
    if(TARGET imgui)
        set(INSTALLER_GUI_AVAILABLE ON)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/cmake/BuildImGui.cmake")
        include("${CMAKE_SOURCE_DIR}/cmake/BuildImGui.cmake")
        if(IMGUI_FOUND AND TARGET imgui)
            set(INSTALLER_GUI_AVAILABLE ON)
        endif()
    endif()
endif()

if(INSTALLER_GUI_AVAILABLE)
    list(APPEND INSTALLER_SOURCES
        src/gui/WizardGui.h
        src/gui/WizardGui.cpp
    )
    if(WIN32)
        list(APPEND INSTALLER_SOURCES src/gui/BackendWindows.cpp)
    else()
        list(APPEND INSTALLER_SOURCES src/gui/BackendLinux.cpp)
    endif()
endif()

add_executable(SparkInstaller ${INSTALLER_SOURCES})

if(WIN32)
    target_sources(SparkInstaller PRIVATE resources/SparkInstaller.rc)
endif()

target_include_directories(SparkInstaller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(SparkInstaller PRIVATE SparkBuildCore)

if(INSTALLER_GUI_AVAILABLE)
    target_compile_definitions(SparkInstaller PRIVATE SPARKINSTALLER_ENABLE_GUI=1)
    target_link_libraries(SparkInstaller PRIVATE imgui)
    if(WIN32)
        # d3dcompiler: imgui_impl_dx11 calls D3DCompile and relies on MSVC's
        # #pragma comment(lib) auto-link, which MinGW ignores — link explicitly.
        target_link_libraries(SparkInstaller PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 shell32 ole32 oleaut32 uuid comdlg32 advapi32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(SparkInstaller PRIVATE Threads::Threads)
    endif()
endif()

if(MSVC)
    # Match imgui's CRT linkage (cmake/BuildImGui.cmake uses the DLL CRT).
    # Mismatched /MT vs /MD causes LNK2019 on __imp_* symbols from imgui.lib.
    set_property(TARGET SparkInstaller PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_definitions(SparkInstaller PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SparkInstaller PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(SparkInstaller PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS SparkInstaller RUNTIME DESTINATION bin COMPONENT tools)
