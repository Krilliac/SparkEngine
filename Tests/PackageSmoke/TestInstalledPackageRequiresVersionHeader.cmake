# An installed package must derive its module ABI from its own SDK headers.
# A consumer-side SparkSDK directory must not make an incomplete package
# appear usable by satisfying the helper's source-tree fallback.
foreach(required IN ITEMS SPARK_GAME_MODULE_CMAKE FIXTURE CONSUMER_GENERATOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required for the installed SDK version-header test")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
set(project_root "${FIXTURE}/project")
set(package_dir "${FIXTURE}/package/lib/cmake/SparkEngine")
set(installed_include "${FIXTURE}/installed/include")
file(MAKE_DIRECTORY
    "${project_root}/SparkSDK/Include/Spark"
    "${package_dir}"
    "${installed_include}/Spark")
file(COPY "${SPARK_GAME_MODULE_CMAKE}" DESTINATION "${package_dir}")

# This is a stale/consumer-local header. The installed include root is
# deliberately missing Version.h even though its Spark directory exists.
file(WRITE "${project_root}/SparkSDK/Include/Spark/Version.h"
    "#pragma once\n#define SPARK_SDK_VERSION 3\n")

file(TO_CMAKE_PATH "${installed_include}" installed_include_cmake)
file(TO_CMAKE_PATH "${package_dir}/SparkGameModule.cmake" package_helper_cmake)
file(WRITE "${project_root}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(SparkMissingInstalledVersion LANGUAGES CXX)\n"
    "set(SPARK_ENGINE_INCLUDE_DIR \"${installed_include_cmake}\")\n"
    "include(\"${package_helper_cmake}\")\n")

set(configure "${CMAKE_COMMAND}" -S "${project_root}" -B "${FIXTURE}/build"
    -G "${CONSUMER_GENERATOR}")
if(CONSUMER_PLATFORM)
    list(APPEND configure -A "${CONSUMER_PLATFORM}")
endif()
if(CONSUMER_TOOLSET)
    list(APPEND configure -T "${CONSUMER_TOOLSET}")
endif()
if(CONSUMER_COMPILER)
    list(APPEND configure "-DCMAKE_CXX_COMPILER=${CONSUMER_COMPILER}")
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
        "Installed SparkEngine helper accepted a package missing its own Spark/Version.h "
        "by falling back to a consumer-local SDK header")
endif()
if(NOT "${output}\n${error}" MATCHES "could not locate Spark/Version[.]h")
    message(FATAL_ERROR
        "Installed SparkEngine helper rejected the incomplete package for an unrelated reason "
        "(${result}):\n${output}\n${error}")
endif()
