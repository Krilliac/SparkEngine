# An installed SDK must reject a package missing its canonical public umbrella
# header during find_package, rather than allowing the failure to surface only
# after a consumer has started compiling.
foreach(required IN ITEMS SPARK_SOURCE_ROOT FIXTURE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required for the public SDK header test")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
set(_prefix "${FIXTURE}/prefix")
set(_package_dir "${_prefix}/lib/cmake/SparkEngine")
set(_consumer_dir "${FIXTURE}/consumer")
file(MAKE_DIRECTORY "${_package_dir}" "${_prefix}/include/Spark" "${_consumer_dir}")

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
file(WRITE "${_package_dir}/SparkEngineConfigVersion.cmake"
    "set(PACKAGE_VERSION \"1.0.0\")\n")
file(WRITE "${_package_dir}/SparkEngineTargets.cmake"
    "if(NOT TARGET Spark::SparkEngineLib)\n"
    "    add_library(Spark::SparkEngineLib INTERFACE IMPORTED)\n"
    "endif()\n")
file(WRITE "${_package_dir}/SparkEngineGameModules.cmake" "# fixture\n")
foreach(_helper IN ITEMS SparkGameModule.cmake SparkPlugin.cmake
                        WriteSparkModuleABI.cmake WriteSparkPluginMetadata.cmake)
    file(COPY "${SPARK_SOURCE_ROOT}/cmake/${_helper}" DESTINATION "${_package_dir}")
endforeach()

# Keep the helper's own inputs present. SparkSDK.h is deliberately omitted.
foreach(_header IN ITEMS Version.h PluginABI.h)
    file(COPY "${SPARK_SOURCE_ROOT}/SparkSDK/Include/Spark/${_header}"
        DESTINATION "${_prefix}/include/Spark")
endforeach()

file(WRITE "${_consumer_dir}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(SparkMissingPublicHeaderConsumer NONE)\n"
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
        "Consumer accepted an SDK package missing Spark/SparkSDK.h:\n"
        "${_output}\n${_error}")
endif()
if(NOT "${_output}\n${_error}" MATCHES "SparkSDK[.]h")
    message(FATAL_ERROR
        "Consumer rejected the incomplete SDK for an unrelated reason (${_result}):\n"
        "${_output}\n${_error}")
endif()
file(REMOVE_RECURSE "${FIXTURE}")
