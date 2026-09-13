# The SDK install component must be self-contained for a consumer that selects
# only the public SDK: headers, exported targets, compatibility helpers, legal
# notices, documentation, and one buildable example must travel together.
foreach(_required IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_CONFIG SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the SDK component completeness test")
    endif()
endforeach()

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
set(_prefix "${SPARK_TEST_ROOT}/prefix")
set(_install_command
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_prefix}" --component sdk)
execute_process(
    COMMAND ${_install_command}
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error
    TIMEOUT 600)
if(NOT "${_install_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Installing the SDK component failed (${_install_result}):\n"
        "${_install_output}\n${_install_error}")
endif()

set(_required_sdk_files
    include/Spark/SparkSDK.h
    lib/cmake/SparkEngine/SparkEngineConfig.cmake
    lib/cmake/SparkEngine/SparkEngineTargets.cmake
    lib/cmake/SparkEngine/SparkGameModule.cmake
    share/SparkEngine/sdk/README.md
    share/SparkEngine/sdk/LICENSE.txt
    share/SparkEngine/sdk/THIRD_PARTY_NOTICES.txt
    share/SparkEngine/sdk/examples/EmptyProject/CMakeLists.txt)
set(_missing_sdk_files)
foreach(_relative_file IN LISTS _required_sdk_files)
    if(NOT EXISTS "${_prefix}/${_relative_file}" OR
       IS_DIRECTORY "${_prefix}/${_relative_file}")
        list(APPEND _missing_sdk_files "${_relative_file}")
    endif()
endforeach()
if(_missing_sdk_files)
    string(REPLACE ";" "\n  " _missing_sdk_report "${_missing_sdk_files}")
    message(FATAL_ERROR
        "SDK component is missing required public-package files:\n"
        "  ${_missing_sdk_report}\n"
        "Install output:\n${_install_output}")
endif()

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
