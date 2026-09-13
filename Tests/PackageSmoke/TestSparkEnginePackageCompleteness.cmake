# An install with no package-version file must not be accepted by a normal
# find_package consumer. Without the version diagnostics, an SDK package can
# silently omit its semantic compatibility contract.
foreach(required IN ITEMS SPARK_SOURCE_ROOT FIXTURE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required for the incomplete SDK package test")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
set(_prefix "${FIXTURE}/prefix")
set(_package_dir "${_prefix}/lib/cmake/SparkEngine")
set(_consumer_dir "${FIXTURE}/consumer")
file(MAKE_DIRECTORY "${_package_dir}" "${_prefix}/include/Spark" "${_consumer_dir}")

# Render the real package configuration so this test exercises the shipped
# find_package contract rather than a reimplementation of it.
include(CMakePackageConfigHelpers)
set(SPARK_PACKAGE_HAS_ANGELSCRIPT 0)
set(SPARK_PACKAGE_HAS_CURL 0)
set(SPARK_PACKAGE_HAS_SDL2 0)
set(SPARK_PACKAGE_HAS_OPENAL 0)
configure_package_config_file(
    "${SPARK_SOURCE_ROOT}/cmake/SparkEngineConfig.cmake.in"
    "${_package_dir}/SparkEngineConfig.cmake"
    INSTALL_DESTINATION lib/cmake/SparkEngine
)

# Keep every other installed companion present so the missing version file is
# the only incomplete-package condition under test.
foreach(_helper IN ITEMS SparkGameModule.cmake SparkPlugin.cmake
                        WriteSparkModuleABI.cmake WriteSparkPluginMetadata.cmake)
    file(COPY "${SPARK_SOURCE_ROOT}/cmake/${_helper}" DESTINATION "${_package_dir}")
endforeach()
file(COPY "${SPARK_SOURCE_ROOT}/SparkSDK/Include/Spark/Version.h"
    DESTINATION "${_prefix}/include/Spark")
file(COPY "${SPARK_SOURCE_ROOT}/SparkSDK/Include/Spark/PluginABI.h"
    DESTINATION "${_prefix}/include/Spark")
file(WRITE "${_package_dir}/SparkEngineTargets.cmake"
    "if(NOT TARGET Spark::SparkEngineLib)\n"
    "    add_library(Spark::SparkEngineLib INTERFACE IMPORTED)\n"
    "endif()\n")
file(WRITE "${_package_dir}/SparkEngineGameModules.cmake" "# fixture\n")

file(WRITE "${_consumer_dir}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(SparkIncompletePackageConsumer NONE)\n"
    "find_package(SparkEngine CONFIG REQUIRED)\n"
    "if(NOT TARGET Spark::SparkEngineLib OR NOT COMMAND spark_add_game_module)\n"
    "    message(FATAL_ERROR \"The package did not expose its consumer contract\")\n"
    "endif()\n")

set(_configure "${CMAKE_COMMAND}" -S "${_consumer_dir}" -B "${FIXTURE}/build"
    "-DSparkEngine_DIR=${_package_dir}")
execute_process(COMMAND ${_configure}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error TIMEOUT 120)
if("${_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Consumer accepted an SDK package missing SparkEngineConfigVersion.cmake:\n"
        "${_output}\n${_error}")
endif()
if(NOT "${_output}\n${_error}" MATCHES "SparkEngineConfigVersion[.]cmake")
    message(FATAL_ERROR
        "Consumer rejected the incomplete SDK package for an unrelated reason (${_result}):\n"
        "${_output}\n${_error}")
endif()
file(REMOVE_RECURSE "${FIXTURE}")
