cmake_minimum_required(VERSION 3.25)

project(SparkLauncher VERSION ${SPARK_ENGINE_VERSION} LANGUAGES CXX)

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
# The launcher is styled by the Spark UI kit, which the root CMakeLists creates (add_subdirectory(SparkUI))
# whenever the imgui target exists.
if(NOT TARGET SparkUIKit)
    message(WARNING "SparkLauncher: SparkUIKit not found — skipping build")
    return()
endif()
if(NOT COMMAND spark_stage_ui_fonts)
    include("${CMAKE_SOURCE_DIR}/cmake/SparkUIFonts.cmake")
endif()

set(LAUNCHER_SOURCES
    src/main.cpp
    src/LauncherApp.cpp
    src/LauncherApp.h
    src/LauncherPaths.cpp
    src/LauncherPaths.h
    src/LauncherProcess.cpp
    src/LauncherProcess.h
    src/LauncherTemplates.cpp
    src/LauncherTemplates.h
    src/LauncherBackend.h
    src/LauncherViews.cpp
    src/LauncherViews.h
)
if(WIN32)
    list(APPEND LAUNCHER_SOURCES src/LauncherBackendWindows.cpp resources/SparkLauncher.rc)
    add_executable(SparkLauncher WIN32 ${LAUNCHER_SOURCES})
    target_link_libraries(SparkLauncher PRIVATE
        d3d11 dxgi user32 gdi32 shell32 ole32 oleaut32 uuid comdlg32 advapi32 dwmapi)
    # resources/SparkLauncher.rc names the logomark icon by file name only.
    target_include_directories(SparkLauncher PRIVATE
        "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_SOURCE_DIR}/Assets/Engine/Branding>")
else()
    list(APPEND LAUNCHER_SOURCES src/LauncherBackendLinux.cpp)
    add_executable(SparkLauncher ${LAUNCHER_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(SparkLauncher PRIVATE Threads::Threads)
endif()

# Pull ProjectManager + FolderDialog directly from the editor tree — no copy, no new lib.
target_sources(SparkLauncher PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/ProjectManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Core/ProjectDocument.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Utils/FolderDialog.cpp"
)

target_include_directories(SparkLauncher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
)

target_link_libraries(SparkLauncher PRIVATE imgui SparkUIKit)
# Spark::Ui::FindFontDirectory looks in <exe dir>/EditorAssets/Fonts; stage them there so a
# launcher-only build tree gets the Ember Signal faces instead of the embedded fallback font.
spark_stage_ui_fonts(SparkLauncher)
if(TARGET SparkEngineLib)
    target_link_libraries(SparkLauncher PRIVATE SparkEngineLib)
endif()

# Match the version macros SparkEditor defines so the shared ProjectManager.cpp compiles here.
target_compile_definitions(SparkLauncher PRIVATE
    SPARK_LAUNCHER_VERSION=\"${PROJECT_VERSION}\"
    EDITOR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    EDITOR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    EDITOR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
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

if(BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME SparkLauncherHelp COMMAND $<TARGET_FILE:SparkLauncher> --help)
    set(_sparklauncher_version_test_script
        "${CMAKE_CURRENT_BINARY_DIR}/VerifySparkLauncherVersion.cmake")
    file(GENERATE OUTPUT "${_sparklauncher_version_test_script}" CONTENT [=[
foreach(_spark_required IN ITEMS
        SPARK_VERSION_EXECUTABLE SPARK_EXPECTED_VERSION_OUTPUT)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "${_spark_required} is required")
    endif()
endforeach()
execute_process(
    COMMAND "${SPARK_VERSION_EXECUTABLE}" --version
    RESULT_VARIABLE _spark_version_result
    OUTPUT_VARIABLE _spark_version_output
    ERROR_VARIABLE _spark_version_error)
if(NOT _spark_version_result EQUAL 0)
    message(FATAL_ERROR
        "Version command failed (${_spark_version_result}): ${_spark_version_error}")
endif()
string(REPLACE "\r\n" "\n" _spark_version_output "${_spark_version_output}")
if(NOT _spark_version_output STREQUAL "${SPARK_EXPECTED_VERSION_OUTPUT}\n")
    message(FATAL_ERROR
        "Version output mismatch:\n"
        "  expected: '${SPARK_EXPECTED_VERSION_OUTPUT}\\n'\n"
        "  actual: '${_spark_version_output}'")
endif()
]=])
    add_test(NAME SparkLauncherVersion
        COMMAND "${CMAKE_COMMAND}"
            "-DSPARK_VERSION_EXECUTABLE=$<TARGET_FILE:SparkLauncher>"
            "-DSPARK_EXPECTED_VERSION_OUTPUT=SparkLauncher ${PROJECT_VERSION}"
            -P "${_sparklauncher_version_test_script}")
    add_test(NAME SparkLauncherHeadlessSmoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/Tests/Tools/run_launcher_headless_smoke.py"
            "$<TARGET_FILE:SparkLauncher>")
    set_tests_properties(SparkLauncherHelp SparkLauncherVersion SparkLauncherHeadlessSmoke PROPERTIES
        TIMEOUT 10
        LABELS "launcher;cli;integration;process")
    unset(_sparklauncher_version_test_script)

    # The faces Spark::Ui::LoadFontSet loads must sit in EditorAssets/Fonts next to the launcher
    # (staged by spark_stage_ui_fonts above). Without them the launcher silently falls back to the
    # embedded default font and drops every icon, so a missing file fails this test.
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/SparkEditor/Fonts")
        set(_sparklauncher_fonts_test_script
            "${CMAKE_CURRENT_BINARY_DIR}/VerifySparkLauncherStagedFonts.cmake")
        file(GENERATE OUTPUT "${_sparklauncher_fonts_test_script}" CONTENT [=[
if(NOT DEFINED SPARK_FONT_DIRECTORY OR "${SPARK_FONT_DIRECTORY}" STREQUAL "")
    message(FATAL_ERROR "SPARK_FONT_DIRECTORY is required")
endif()
set(_spark_missing_fonts "")
foreach(_spark_font IN ITEMS
        IBMPlexSans-Regular.ttf IBMPlexSans-SemiBold.ttf IBMPlexSans-Bold.ttf
        JetBrainsMono-Regular.ttf fa-solid-900.ttf)
    if(NOT EXISTS "${SPARK_FONT_DIRECTORY}/${_spark_font}")
        list(APPEND _spark_missing_fonts "${_spark_font}")
    endif()
endforeach()
if(_spark_missing_fonts)
    message(FATAL_ERROR
        "Fonts missing from ${SPARK_FONT_DIRECTORY}: ${_spark_missing_fonts}")
endif()
message(STATUS "SparkLauncher fonts staged in ${SPARK_FONT_DIRECTORY}")
]=])
        add_test(NAME SparkLauncherStagedFonts
            COMMAND "${CMAKE_COMMAND}"
                "-DSPARK_FONT_DIRECTORY=$<TARGET_FILE_DIR:SparkLauncher>/EditorAssets/Fonts"
                -P "${_sparklauncher_fonts_test_script}")
        set_tests_properties(SparkLauncherStagedFonts PROPERTIES
            TIMEOUT 10
            LABELS "launcher;integration")
        unset(_sparklauncher_fonts_test_script)
    endif()
endif()
