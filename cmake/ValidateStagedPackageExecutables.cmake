# Validate the executable bill of materials for SparkEngine's default release
# configuration. Run after `cmake --install` and before CPack uploads artifacts.

if(NOT DEFINED SPARK_PACKAGE_ROOT OR SPARK_PACKAGE_ROOT STREQUAL "")
    message(FATAL_ERROR "SPARK_PACKAGE_ROOT must name the staged install root")
endif()

if(NOT DEFINED SPARK_EXECUTABLE_SUFFIX)
    if(CMAKE_HOST_WIN32)
        set(SPARK_EXECUTABLE_SUFFIX ".exe")
    else()
        set(SPARK_EXECUTABLE_SUFFIX "")
    endif()
endif()

set(_spark_required_executables
    SparkEngine
    SparkConsole
    SparkEditor
    SparkLauncher
    SparkServer
    SparkGateway
    SparkDaemon
    SparkCollabServer
    SparkOrchestrator
    SparkCooker
    SparkWorker
    SparkAutomation
    SparkBuild
    SparkInstaller
    SparkShaderCompiler
    SparkCrashReporter
)

set(_spark_missing_executables "")
foreach(_spark_executable IN LISTS _spark_required_executables)
    set(_spark_path
        "${SPARK_PACKAGE_ROOT}/bin/${_spark_executable}${SPARK_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${_spark_path}" OR IS_DIRECTORY "${_spark_path}")
        list(APPEND _spark_missing_executables "${_spark_path}")
    endif()
endforeach()

if(_spark_missing_executables)
    list(JOIN _spark_missing_executables "\n  " _spark_missing_report)
    message(FATAL_ERROR
        "Staged SparkEngine package is missing required executables:\n"
        "  ${_spark_missing_report}")
endif()

set(_spark_required_runtime_files
    LICENSE.txt
    THIRD_PARTY_NOTICES.txt
    bin/Shaders/BasicVS.hlsl
    bin/Shaders/HLSL/BasicVS.hlsl
    bin/Shaders/HLSL/Compute/GPUCull.hlsl
    bin/Assets/MMOFPS/Data/continents.json
    bin/Assets/Engine/Branding/sparkengine_wordmark.svg
    bin/Resources/Config/settings.ini
    bin/Resources/Config/controls.cfg
)
set(_spark_missing_runtime_files "")
foreach(_spark_relative_path IN LISTS _spark_required_runtime_files)
    if(NOT EXISTS "${SPARK_PACKAGE_ROOT}/${_spark_relative_path}")
        list(APPEND _spark_missing_runtime_files
            "${SPARK_PACKAGE_ROOT}/${_spark_relative_path}")
    endif()
endforeach()
if(_spark_missing_runtime_files)
    list(JOIN _spark_missing_runtime_files "\n  " _spark_missing_runtime_report)
    message(FATAL_ERROR
        "Staged SparkEngine package is missing required runtime content:\n"
        "  ${_spark_missing_runtime_report}")
endif()

set(_spark_help_smoke_executables
    SparkEngine
    SparkLauncher
    SparkServer
    SparkGateway
    SparkDaemon
    SparkCollabServer
    SparkOrchestrator
    SparkCooker
    SparkWorker
    SparkAutomation
    SparkBuild
    SparkInstaller
    SparkShaderCompiler
    SparkCrashReporter
)
foreach(_spark_executable IN LISTS _spark_help_smoke_executables)
    set(_spark_help_path
        "${SPARK_PACKAGE_ROOT}/bin/${_spark_executable}${SPARK_EXECUTABLE_SUFFIX}")
    execute_process(
        COMMAND "${_spark_help_path}" --help
        RESULT_VARIABLE _spark_help_result
        OUTPUT_VARIABLE _spark_help_stdout
        ERROR_VARIABLE _spark_help_stderr
        TIMEOUT 10
    )
    if(NOT _spark_help_result EQUAL 0)
        message(FATAL_ERROR
            "Staged ${_spark_executable} --help smoke failed (exit ${_spark_help_result})\n"
            "stdout:\n${_spark_help_stdout}\n"
            "stderr:\n${_spark_help_stderr}")
    endif()
endforeach()

execute_process(
    COMMAND "${SPARK_PACKAGE_ROOT}/bin/SparkEngine${SPARK_EXECUTABLE_SUFFIX}" --version
    RESULT_VARIABLE _spark_version_result
    OUTPUT_VARIABLE _spark_version_stdout
    ERROR_VARIABLE _spark_version_stderr
    TIMEOUT 10
)
if(NOT _spark_version_result EQUAL 0 OR
   NOT _spark_version_stdout MATCHES "SparkEngine [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR
        "Staged SparkEngine --version smoke failed (exit ${_spark_version_result})\n"
        "stdout:\n${_spark_version_stdout}\n"
        "stderr:\n${_spark_version_stderr}")
endif()

list(LENGTH _spark_required_executables _spark_executable_count)
message(STATUS
    "Validated ${_spark_executable_count} required executables in ${SPARK_PACKAGE_ROOT}/bin")
