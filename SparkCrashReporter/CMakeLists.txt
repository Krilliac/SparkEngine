cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SPARK_ENGINE_VERSION OR SPARK_ENGINE_VERSION STREQUAL "")
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/SparkWindowsVersionInfo.cmake")
    spark_resolve_engine_version(SPARK_ENGINE_VERSION)
endif()
project(SparkCrashReporter VERSION ${SPARK_ENGINE_VERSION})

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
        SPARK_CRASH_REPORTER_VERSION=\"${PROJECT_VERSION}\"
    )
    if(WIN32)
        target_compile_definitions(SparkCrashReporter PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            SPARK_PLATFORM_WINDOWS
        )
        target_link_libraries(SparkCrashReporter PRIVATE
            kernel32 user32 bcrypt
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

        add_executable(SparkCrashReporterFakeGh tests/FakeGh.cpp)

        add_executable(SparkCrashReporterProductionTimeoutTests
            tests/CrashReporterProductionTimeoutTests.cpp
            src/CrashAutoIssues.cpp
            src/CrashAutoIssues.h
        )
        add_dependencies(SparkCrashReporterProductionTimeoutTests SparkCrashReporterFakeGh)
        target_include_directories(SparkCrashReporterProductionTimeoutTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(SparkCrashReporterProductionTimeoutTests PRIVATE
            SPARK_CRASH_REPORTER_VERSION="${PROJECT_VERSION}"
        )
        if(WIN32)
            target_compile_definitions(SparkCrashReporterProductionTimeoutTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
            target_link_libraries(SparkCrashReporterProductionTimeoutTests PRIVATE kernel32 user32 bcrypt)
        else()
            target_link_libraries(SparkCrashReporterProductionTimeoutTests PRIVATE Threads::Threads)
        endif()
        add_test(NAME SparkCrashReporter.ProductionTimeout
                 COMMAND SparkCrashReporterProductionTimeoutTests $<TARGET_FILE:SparkCrashReporterFakeGh>)
        set_tests_properties(SparkCrashReporter.ProductionTimeout PROPERTIES
                             TIMEOUT 5
                             LABELS "crash-reporter;unit")

        add_executable(SparkCrashReporterManifestTests
            tests/CrashReporterManifestTests.cpp
            src/CrashReporterApp.cpp
            src/CrashReporterApp.h
            src/CrashAutoIssues.cpp
            src/CrashAutoIssues.h
        )
        target_include_directories(SparkCrashReporterManifestTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../SparkEngine/Source
        )
        target_compile_definitions(SparkCrashReporterManifestTests PRIVATE
            _CRT_SECURE_NO_WARNINGS
            SPARK_CRASH_REPORTER_VERSION="${PROJECT_VERSION}"
            SPARK_AUTO_ISSUE_TEST_TIMEOUT_MS=400
        )
        if(WIN32)
            target_compile_definitions(SparkCrashReporterManifestTests PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                SPARK_PLATFORM_WINDOWS
            )
            target_link_libraries(SparkCrashReporterManifestTests PRIVATE
                kernel32 user32 advapi32 bcrypt
            )
        else()
            target_link_libraries(SparkCrashReporterManifestTests PRIVATE Threads::Threads)
        endif()

        add_test(NAME SparkCrashReporter.ManifestCompatibility
                 COMMAND SparkCrashReporterManifestTests $<TARGET_FILE:SparkCrashReporterFakeGh>)
        # OPS-100: path, identity, size, transport-field and read-only defenses belong
        # to the crash-security selection on every platform, not only consent.
        set_tests_properties(SparkCrashReporter.ManifestCompatibility PROPERTIES
                             TIMEOUT 60
                             LABELS "crash-security;security;unit")

        # OPS-100: an unanswered or non-explicit consent prompt transmits nothing.
        add_executable(SparkCrashReporterConsentTests
            tests/CrashReporterConsentTests.cpp
            src/CrashReporterApp.cpp
            src/CrashReporterApp.h
            src/CrashAutoIssues.cpp
            src/CrashAutoIssues.h
        )
        add_dependencies(SparkCrashReporterConsentTests SparkCrashReporterFakeGh)
        target_include_directories(SparkCrashReporterConsentTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(SparkCrashReporterConsentTests PRIVATE
            _CRT_SECURE_NO_WARNINGS
            SPARK_CRASH_REPORTER_VERSION="${PROJECT_VERSION}"
            SPARK_AUTO_ISSUE_TEST_TIMEOUT_MS=400
        )
        if(WIN32)
            target_compile_definitions(SparkCrashReporterConsentTests PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                SPARK_PLATFORM_WINDOWS
            )
            target_link_libraries(SparkCrashReporterConsentTests PRIVATE kernel32 user32 advapi32 bcrypt)
        else()
            target_link_libraries(SparkCrashReporterConsentTests PRIVATE Threads::Threads)
        endif()
        add_test(NAME SparkCrashReporter.CrashReporter_Consent
                 COMMAND SparkCrashReporterConsentTests $<TARGET_FILE:SparkCrashReporterFakeGh>)
        set_tests_properties(SparkCrashReporter.CrashReporter_Consent PROPERTIES
                             TIMEOUT 30
                             LABELS "crash-security;security;unit")

        add_test(NAME SparkCrashReporter.ExecutableSmoke
                 COMMAND ${CMAKE_COMMAND}
                         -DREPORTER=$<TARGET_FILE:SparkCrashReporter>
                         -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/spark-crash-reporter-executable-smoke
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/CrashReporterExecutableSmoke.cmake)
        set_tests_properties(SparkCrashReporter.ExecutableSmoke PROPERTIES
                             TIMEOUT 30
                             LABELS "crash-reporter;integration;process")
    endif()
else()
    message(STATUS "SparkCrashReporter: no source files found, skipping")
endif()
