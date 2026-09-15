cmake_minimum_required(VERSION 3.25)

project(SparkConsole)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the same output directories as parent project
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Ensure all configurations use the same output directory
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER})
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endif()
endforeach()

# Find all source files in SparkConsole directory
file(GLOB_RECURSE CONSOLE_SOURCES
    CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp"
)

# Only create the target if we have source files
if(CONSOLE_SOURCES)
    # Create the console executable
    add_executable(SparkConsole ${CONSOLE_SOURCES})

    # Include directories
    target_include_directories(SparkConsole PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Compiler definitions
    if(NOT DEFINED SPARK_ENGINE_VERSION OR SPARK_ENGINE_VERSION STREQUAL "")
        message(FATAL_ERROR "SparkConsole requires the top-level SPARK_ENGINE_VERSION")
    endif()
    target_compile_definitions(SparkConsole PRIVATE
        CONSOLE_ENABLED
        SPARK_CONSOLE_VERSION=\"${SPARK_ENGINE_VERSION}\"
        SPARK_CONSOLE_VERSION_W=L\"${SPARK_ENGINE_VERSION}\"
    )
    if(WIN32)
        target_compile_definitions(SparkConsole PRIVATE
            _CRT_SECURE_NO_WARNINGS
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            SPARK_PLATFORM_WINDOWS
        )
    endif()

    # Per-configuration definitions so console compiles in both Debug and Release
    target_compile_definitions(SparkConsole PRIVATE
        $<$<CONFIG:Debug>:SPARK_DEBUG=1>
        $<$<CONFIG:Debug>:SPARK_CONSOLE_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:SPARK_DEVELOPMENT=1>
        $<$<CONFIG:Release>:SPARK_RELEASE=1>
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:MinSizeRel>:SPARK_SHIPPING=1>
        $<$<CONFIG:MinSizeRel>:NDEBUG>
    )

    # System libraries - platform-specific
    if(WIN32)
        target_link_libraries(SparkConsole PRIVATE
            kernel32 user32 gdi32
            shell32 comdlg32 advapi32
            winmm
        )
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(SparkConsole PRIVATE Threads::Threads)
        if(APPLE)
            target_compile_definitions(SparkConsole PRIVATE SPARK_PLATFORM_MACOS)
        else()
            target_compile_definitions(SparkConsole PRIVATE SPARK_PLATFORM_LINUX)
        endif()
    endif()

    # MSVC specific settings - works for both Debug and Release
    if(MSVC)
        target_compile_options(SparkConsole PRIVATE
            /W3 /MP
            /wd4996
            # Debug-info format comes from CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
            # (Embedded, forced in the top-level CMakeLists.txt); no per-target /Zi.
            $<$<CONFIG:Debug>:/Od /RTC1>
            $<$<CONFIG:Release>:/O2 /Oi /GL>
            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<CONFIG:MinSizeRel>:/O1 /Os>
        )
        # Enable whole-program optimization for Release
        target_link_options(SparkConsole PRIVATE
            $<$<CONFIG:Release>:/LTCG>
        )
        set_property(TARGET SparkConsole PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

        # Organize source files in Visual Studio
        foreach(src ${CONSOLE_SOURCES})
            get_filename_component(dir "${src}" DIRECTORY)
            file(RELATIVE_PATH grp "${CMAKE_CURRENT_SOURCE_DIR}/src" "${dir}")
            string(REPLACE "/" "\\\\" grp "${grp}")
            if(grp STREQUAL "")
                source_group("Source Files" FILES "${src}")
            else()
                source_group("Source Files\\\\${grp}" FILES "${src}")
            endif()
        endforeach()
    endif()

    # GCC/Clang per-configuration flags
    if(NOT MSVC)
        target_compile_options(SparkConsole PRIVATE
            -Wall -Wextra -Wno-unused-parameter
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O2 -DNDEBUG>
            $<$<CONFIG:RelWithDebInfo>:-g -O2>
            $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
        )
    endif()

    # SparkEngine launches this companion process at runtime, so it must be
    # present in installed trees and in every CPack runtime package.
    install(TARGETS SparkConsole
        RUNTIME DESTINATION bin
        COMPONENT runtime
    )

    if(BUILD_TESTS)
        enable_testing()
        set(_sparkconsole_version_test_script
            "${CMAKE_CURRENT_BINARY_DIR}/VerifySparkConsoleVersion.cmake")
        file(GENERATE OUTPUT "${_sparkconsole_version_test_script}" CONTENT [=[
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
        add_test(NAME SparkConsoleVersion
            COMMAND "${CMAKE_COMMAND}"
                "-DSPARK_VERSION_EXECUTABLE=$<TARGET_FILE:SparkConsole>"
                "-DSPARK_EXPECTED_VERSION_OUTPUT=SparkConsole ${SPARK_ENGINE_VERSION}"
                -P "${_sparkconsole_version_test_script}")
        set_tests_properties(SparkConsoleVersion PROPERTIES TIMEOUT 10)
        unset(_sparkconsole_version_test_script)
    endif()

    message(STATUS "SparkConsole configured successfully (all configurations: Debug, Release, RelWithDebInfo, MinSizeRel)")
    message(STATUS "SparkConsole output directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
else()
    message(STATUS "No SparkConsole source files found - skipping SparkConsole build")
endif()
