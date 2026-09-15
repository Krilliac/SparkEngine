# Stage the configured Windows package and run the real stable-v1 runtime
# validator against the installed SparkGameFPS payload. This is a package
# smoke slice; it does not certify the full single-player gameplay contract.

foreach(_required IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the installed FPS package test")
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
set(_expected_manifest "${SPARK_ENGINE_BUILD_DIR}/SparkEngineGameModules.cmake")

_run_checked("Install configured FPS runtime component"
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component runtime)
_run_checked("Install configured FPS module component"
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component samples)

if(NOT EXISTS "${_expected_manifest}" OR IS_DIRECTORY "${_expected_manifest}")
    message(FATAL_ERROR
        "Configured build is missing its generated game-module manifest: ${_expected_manifest}")
endif()

_run_checked("Validate installed FPS runtime package"
    "${CMAKE_COMMAND}"
    "-DSPARK_PACKAGE_ROOT=${_install_root}"
    "-DSPARK_PACKAGE_LAYOUT=runtime"
    "-DSPARK_PACKAGE_PROFILE=stable-v1"
    "-DSPARK_PACKAGE_VALIDATE_MODULES_ONLY=ON"
    "-DSPARK_PACKAGE_EXPECTED_MODULE_MANIFEST=${_expected_manifest}"
    "-DSPARK_EXECUTABLE_SUFFIX=.exe"
    -P "${SPARK_SOURCE_ROOT}/cmake/ValidateStagedPackageExecutables.cmake")

message(STATUS "Installed SparkGameFPS runtime package smoke completed")
