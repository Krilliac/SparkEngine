cmake_minimum_required(VERSION 3.25)

project(SparkCrashReporter)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the same output directories as parent project
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER})
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endif()
endforeach()

file(GLOB_RECURSE REPORTER_SOURCES
    CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)

if(REPORTER_SOURCES)
    add_executable(SparkCrashReporter ${REPORTER_SOURCES})

    target_include_directories(SparkCrashReporter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_definitions(SparkCrashReporter PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    if(WIN32)
        target_compile_definitions(SparkCrashReporter PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            SPARK_PLATFORM_WINDOWS
        )
        target_link_libraries(SparkCrashReporter PRIVATE
            kernel32 user32
        )
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(SparkCrashReporter PRIVATE Threads::Threads)
    endif()

    # Link libcurl if available (for direct upload support)
    if(CURL_FOUND)
        target_link_libraries(SparkCrashReporter PRIVATE CURL::libcurl)
        target_compile_definitions(SparkCrashReporter PRIVATE SPARK_HAS_CURL)
    endif()

    message(STATUS "SparkCrashReporter will be built")

    if(BUILD_TESTS)
        enable_testing()

        add_executable(SparkCrashReporterManifestTests
            tests/CrashReporterManifestTests.cpp
            src/CrashReporterApp.cpp
            src/CrashReporterApp.h
        )
        target_include_directories(SparkCrashReporterManifestTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_definitions(SparkCrashReporterManifestTests PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
        if(WIN32)
            target_compile_definitions(SparkCrashReporterManifestTests PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                SPARK_PLATFORM_WINDOWS
            )
            target_link_libraries(SparkCrashReporterManifestTests PRIVATE
                kernel32 user32
            )
        else()
            target_link_libraries(SparkCrashReporterManifestTests PRIVATE Threads::Threads)
        endif()

        add_test(NAME SparkCrashReporter.ManifestCompatibility
                 COMMAND SparkCrashReporterManifestTests)
    endif()
else()
    message(STATUS "SparkCrashReporter: no source files found, skipping")
endif()
