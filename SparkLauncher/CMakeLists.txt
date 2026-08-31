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

set(LAUNCHER_SOURCES
    src/main.cpp
    src/LauncherApp.cpp
    src/LauncherApp.h
    src/LauncherPaths.cpp
    src/LauncherPaths.h
    src/LauncherProcess.cpp
    src/LauncherProcess.h
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
    set_tests_properties(SparkLauncherHelp SparkLauncherVersion PROPERTIES TIMEOUT 10)
    unset(_sparklauncher_version_test_script)
endif()
