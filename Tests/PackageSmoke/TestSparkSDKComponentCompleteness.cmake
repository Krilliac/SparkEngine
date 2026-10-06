# Install the SDK's declared runtime dependency before checking its public
# package: headers, exported targets, compatibility helpers, legal notices,
# documentation, and one buildable example must travel together.
foreach(_required IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_CONFIG SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the SDK component completeness test")
    endif()
endforeach()

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
set(_prefix "${SPARK_TEST_ROOT}/prefix")
foreach(_component IN ITEMS runtime sdk)
    set(_install_command
        "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
        --config "${SPARK_CONFIG}" --prefix "${_prefix}" --component "${_component}")
    execute_process(
        COMMAND ${_install_command}
        RESULT_VARIABLE _install_result
        OUTPUT_VARIABLE _install_output
        ERROR_VARIABLE _install_error
        TIMEOUT 600)
    if(NOT "${_install_result}" STREQUAL "0")
        message(FATAL_ERROR
            "Installing the ${_component} component failed (${_install_result}):\n"
            "${_install_output}\n${_install_error}")
    endif()
endforeach()

set(_required_sdk_files
    include/Spark/SparkSDK.h
    include/Spark/Version.h
    include/Spark/GeneratedVersion.h
    include/Spark/IModule.h
    include/Spark/ModuleABI.h
    include/Spark/IWeatherService.h
    lib/cmake/SparkEngine/SparkEngineConfig.cmake
    lib/cmake/SparkEngine/SparkEngineConfigVersion.cmake
    lib/cmake/SparkEngine/SparkEngineTargets.cmake
    lib/cmake/SparkEngine/SparkGameModule.cmake
    share/SparkEngine/sdk/README.md
    share/SparkEngine/sdk/API-REFERENCE.md
    share/SparkEngine/sdk/MIGRATION.md
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

set(_required_sdk_documentation
    "README.md|## Public headers and ABI"
    "README.md|## Package contents"
    "README.md|## Compatibility diagnostics"
    "README.md|SPARK_SDK_VERSION"
    "README.md|there is no N-1 load or migration path"
    "API-REFERENCE.md|## Module lifecycle"
    "API-REFERENCE.md|SparkModuleCompatibilityDescriptor"
    "MIGRATION.md|## Current version: SDK ABI v10"
    "MIGRATION.md|There is no N-1 module load")
set(_missing_sdk_documentation)
foreach(_required_documentation IN LISTS _required_sdk_documentation)
    string(REPLACE "|" ";" _documentation_parts "${_required_documentation}")
    list(GET _documentation_parts 0 _documentation_file)
    list(GET _documentation_parts 1 _required_text)
    file(READ "${_prefix}/share/SparkEngine/sdk/${_documentation_file}" _documentation_text)
    string(REGEX REPLACE "[ \t\r\n]+" " " _documentation_normalized "${_documentation_text}")
    string(FIND "${_documentation_normalized}" "${_required_text}" _documentation_at)
    if(_documentation_at EQUAL -1)
        list(APPEND _missing_sdk_documentation "${_documentation_file}: ${_required_text}")
    endif()
endforeach()
if(_missing_sdk_documentation)
    string(REPLACE ";" "\n  " _missing_documentation_report "${_missing_sdk_documentation}")
    message(FATAL_ERROR
        "SDK documentation is missing required API/migration guidance:\n"
        "  ${_missing_documentation_report}")
endif()

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
