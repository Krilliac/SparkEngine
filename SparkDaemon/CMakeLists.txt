cmake_minimum_required(VERSION 3.25)

project(SparkDaemon)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# Collaboration accepts a different trust class of traffic and intentionally
# runs in its own process. Keep its implementation out of SparkDaemon.
list(FILTER DAEMON_SOURCES EXCLUDE REGEX ".*/Collaboration.*")
list(FILTER DAEMON_SOURCES EXCLUDE REGEX ".*/CollabServerMain\\.cpp$")
list(FILTER DAEMON_SOURCES EXCLUDE REGEX ".*/OrchestratorMain\\.cpp$")
list(FILTER DAEMON_SOURCES EXCLUDE REGEX ".*/OrchestratorIdentity\\.cpp$")

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

if(WIN32)
    target_compile_definitions(SparkDaemon PRIVATE SPARK_PLATFORM_WINDOWS)
    target_link_libraries(SparkDaemon PRIVATE advapi32)
elseif(APPLE)
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

if(MSVC)
    target_compile_options(SparkDaemon PRIVATE /W4 /permissive- /EHsc /wd4702)
    target_link_options(SparkDaemon PRIVATE /INCREMENTAL:NO)
else()
target_compile_options(SparkDaemon PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-g -O2>
    $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
)
endif()

add_executable(SparkCollabServer
    src/CollabServerMain.cpp
    src/CollaborationService.cpp
    src/DaemonServer.cpp
    src/ControlService.cpp
)
target_include_directories(SparkCollabServer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source
)
target_link_libraries(SparkCollabServer PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(SparkCollabServer PRIVATE SPARK_PLATFORM_WINDOWS)
    target_link_libraries(SparkCollabServer PRIVATE advapi32)
elseif(APPLE)
    target_compile_definitions(SparkCollabServer PRIVATE SPARK_PLATFORM_MACOS)
else()
    target_compile_definitions(SparkCollabServer PRIVATE SPARK_PLATFORM_LINUX)
endif()
target_compile_definitions(SparkCollabServer PRIVATE
    $<$<CONFIG:Debug>:SPARK_DEBUG=1>
    $<$<CONFIG:Release>:SPARK_RELEASE=1 NDEBUG>
    $<$<CONFIG:RelWithDebInfo>:SPARK_DEVELOPMENT=1>
    $<$<CONFIG:MinSizeRel>:SPARK_SHIPPING=1 NDEBUG>
)
if(MSVC)
    target_compile_options(SparkCollabServer PRIVATE /W4 /permissive- /EHsc)
    target_link_options(SparkCollabServer PRIVATE /INCREMENTAL:NO)
else()
target_compile_options(SparkCollabServer PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-g -O2>
    $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
)
endif()

# Operator client with a locked persistent replay identity. It has no process
# handles or signal/kill path; all authority remains in SparkDaemon.
add_executable(SparkOrchestrator
    src/OrchestratorMain.cpp
    src/OrchestratorIdentity.cpp
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source/Utils/DaemonClient.cpp
)
target_include_directories(SparkOrchestrator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source
)
target_link_libraries(SparkOrchestrator PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(SparkOrchestrator PRIVATE SPARK_PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(SparkOrchestrator PRIVATE SPARK_PLATFORM_MACOS)
else()
    target_compile_definitions(SparkOrchestrator PRIVATE SPARK_PLATFORM_LINUX)
endif()
if(MSVC)
    target_compile_options(SparkOrchestrator PRIVATE /W4 /permissive- /EHsc)
    target_link_options(SparkOrchestrator PRIVATE /INCREMENTAL:NO)
else()
    target_compile_options(SparkOrchestrator PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_executable(SparkDaemonServiceTests
        tests/ServiceTests.cpp
        src/CollaborationService.cpp
        src/OrchestratorIdentity.cpp
        src/OrchestrationJournal.cpp
        src/OrchestrationService.cpp
    )
    target_include_directories(SparkDaemonServiceTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/SparkEngine/Source
    )
    target_link_libraries(SparkDaemonServiceTests PRIVATE Threads::Threads)
    if(WIN32)
        target_compile_definitions(SparkDaemonServiceTests PRIVATE SPARK_PLATFORM_WINDOWS)
        target_link_libraries(SparkDaemonServiceTests PRIVATE advapi32)
    elseif(APPLE)
        target_compile_definitions(SparkDaemonServiceTests PRIVATE SPARK_PLATFORM_MACOS)
    else()
        target_compile_definitions(SparkDaemonServiceTests PRIVATE SPARK_PLATFORM_LINUX)
    endif()
    if(MSVC)
        target_compile_options(SparkDaemonServiceTests PRIVATE /W4 /WX /permissive- /EHsc)
        target_link_options(SparkDaemonServiceTests PRIVATE /INCREMENTAL:NO)
    else()
        target_compile_options(SparkDaemonServiceTests PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME SparkDaemonServiceTests COMMAND SparkDaemonServiceTests)
endif()

install(TARGETS SparkDaemon SparkCollabServer SparkOrchestrator
    RUNTIME DESTINATION bin
    COMPONENT runtime
)

message(STATUS "SparkDaemon configured (output: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})")
