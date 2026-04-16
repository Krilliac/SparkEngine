cmake_minimum_required(VERSION 3.25)

project(SparkDaemon)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Phase 1 uses AF_UNIX sockets — POSIX-only for now. A Windows named-pipe
# transport lands with the first Windows-shipping service.
if(WIN32)
    message(STATUS "SparkDaemon: skipped on Windows (named-pipe transport not yet implemented)")
    return()
endif()

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER})
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endif()
endforeach()

file(GLOB_RECURSE DAEMON_SOURCES
    CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp"
)

if(NOT DAEMON_SOURCES)
    message(STATUS "No SparkDaemon source files found - skipping SparkDaemon build")
    return()
endif()

add_executable(SparkDaemon ${DAEMON_SOURCES})

target_include_directories(SparkDaemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source
)

find_package(Threads REQUIRED)
target_link_libraries(SparkDaemon PRIVATE Threads::Threads)

if(APPLE)
    target_compile_definitions(SparkDaemon PRIVATE SPARK_PLATFORM_MACOS)
else()
    target_compile_definitions(SparkDaemon PRIVATE SPARK_PLATFORM_LINUX)
endif()

target_compile_definitions(SparkDaemon PRIVATE
    $<$<CONFIG:Debug>:SPARK_DEBUG=1>
    $<$<CONFIG:Release>:SPARK_RELEASE=1 NDEBUG>
    $<$<CONFIG:RelWithDebInfo>:SPARK_DEVELOPMENT=1>
    $<$<CONFIG:MinSizeRel>:SPARK_SHIPPING=1 NDEBUG>
)

target_compile_options(SparkDaemon PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-g -O2>
    $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
)

message(STATUS "SparkDaemon configured (output: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})")
