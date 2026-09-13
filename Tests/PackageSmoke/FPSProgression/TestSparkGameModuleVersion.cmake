# An SDK header with conflicting ABI definitions must fail before a module
# target can be configured. Accepting one of the definitions would make the
# generated sidecar disagree with the headers used by the consumer.
foreach(required IN ITEMS SPARK_GAME_MODULE_CMAKE FIXTURE CONSUMER_GENERATOR CONSUMER_COMPILER)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required for the ambiguous SDK version test")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
file(MAKE_DIRECTORY "${FIXTURE}/project/SparkSDK/Include/Spark")
file(WRITE "${FIXTURE}/project/SparkSDK/Include/Spark/Version.h"
    "#pragma once\n#define SPARK_SDK_VERSION 4\n#define SPARK_SDK_VERSION 3\n")
file(WRITE "${FIXTURE}/project/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(SparkAmbiguousSDKVersion LANGUAGES CXX)\n"
    "include(\"${SPARK_GAME_MODULE_CMAKE}\")\n")

set(configure "${CMAKE_COMMAND}" -S "${FIXTURE}/project" -B "${FIXTURE}/build"
    -G "${CONSUMER_GENERATOR}"
    "-DSPARK_ENGINE_INCLUDE_DIR=${FIXTURE}/project/SparkSDK/Include"
    "-DCMAKE_CXX_COMPILER=${CONSUMER_COMPILER}")
if(CONSUMER_PLATFORM)
    list(APPEND configure -A "${CONSUMER_PLATFORM}")
endif()
if(CONSUMER_TOOLSET)
    list(APPEND configure -T "${CONSUMER_TOOLSET}")
endif()
if(CONSUMER_MAKE_PROGRAM)
    list(APPEND configure "-DCMAKE_MAKE_PROGRAM=${CONSUMER_MAKE_PROGRAM}")
endif()
if(CONSUMER_TOOLCHAIN)
    list(APPEND configure "-DCMAKE_TOOLCHAIN_FILE=${CONSUMER_TOOLCHAIN}")
endif()

execute_process(COMMAND ${configure}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
file(REMOVE_RECURSE "${FIXTURE}")
if("${result}" STREQUAL "0")
    message(FATAL_ERROR
        "SparkGameModule accepted an SDK header with multiple SPARK_SDK_VERSION definitions")
endif()
if(NOT "${output}\n${error}" MATCHES "exactly one SPARK_SDK_VERSION")
    message(FATAL_ERROR
        "SparkGameModule rejected the ambiguous header for an unrelated reason (${result}):\n"
        "${output}\n${error}")
endif()
