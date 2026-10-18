# Install the configured package, then build and run the real package-smoke
# consumers against only the installed package. The FPS consumers compile the
# production module entrypoint header plus bounded source slices with staged
# public SDK headers; this is not proof that the complete SparkGameFPS DLL is
# public-SDK-only.

foreach(_required IN ITEMS
        SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT
        SPARK_CONSUMER_GENERATOR SPARK_CONSUMER_CTEST)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the installed FPS SDK test")
    endif()
endforeach()

function(_run_checked _stage)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT 600)
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${_stage} failed (${_result}):\n${_output}\n${_error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
set(_install_root "${SPARK_TEST_ROOT}/install")
set(_consumer_build "${SPARK_TEST_ROOT}/consumer-build")
set(_consumer_source "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke")
set(_package_dir "${_install_root}/lib/cmake/SparkEngine")
set(_consumer_config "Release")

_run_checked("Install configured FPS SDK component"
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component sdk)

set(_configure
    "${CMAKE_COMMAND}" -S "${_consumer_source}" -B "${_consumer_build}"
    -G "${SPARK_CONSUMER_GENERATOR}"
    "-DSparkEngine_DIR=${_package_dir}"
    "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=")
if(DEFINED SPARK_CONSUMER_PLATFORM AND NOT "${SPARK_CONSUMER_PLATFORM}" STREQUAL "")
    list(APPEND _configure -A "${SPARK_CONSUMER_PLATFORM}")
endif()
if(DEFINED SPARK_CONSUMER_TOOLSET AND NOT "${SPARK_CONSUMER_TOOLSET}" STREQUAL "")
    list(APPEND _configure -T "${SPARK_CONSUMER_TOOLSET}")
endif()
if(DEFINED SPARK_CONSUMER_COMPILER AND NOT "${SPARK_CONSUMER_COMPILER}" STREQUAL "")
    list(APPEND _configure "-DCMAKE_CXX_COMPILER=${SPARK_CONSUMER_COMPILER}")
endif()
if(DEFINED SPARK_CONSUMER_MAKE_PROGRAM AND NOT "${SPARK_CONSUMER_MAKE_PROGRAM}" STREQUAL "")
    list(APPEND _configure "-DCMAKE_MAKE_PROGRAM=${SPARK_CONSUMER_MAKE_PROGRAM}")
endif()
if(DEFINED SPARK_CONSUMER_TOOLCHAIN AND NOT "${SPARK_CONSUMER_TOOLCHAIN}" STREQUAL "")
    list(APPEND _configure "-DCMAKE_TOOLCHAIN_FILE=${SPARK_CONSUMER_TOOLCHAIN}")
endif()

_run_checked("Configure installed FPS SDK consumers" ${_configure})
_run_checked("Build installed FPS SDK consumers"
    "${CMAKE_COMMAND}" --build "${_consumer_build}"
    --config "${_consumer_config}" --parallel 2)

set(_test "${SPARK_CONSUMER_CTEST}" --test-dir "${_consumer_build}"
    -C "${_consumer_config}" --output-on-failure --no-tests=error)
_run_checked("Run installed FPS SDK consumers" ${_test})

message(STATUS "Installed SparkGameFPS public SDK consumer smoke completed")
