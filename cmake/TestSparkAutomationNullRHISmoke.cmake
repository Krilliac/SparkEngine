cmake_minimum_required(VERSION 3.25)

foreach(_required SPARK_AUTOMATION SPARK_ENGINE SPARK_GAME SPARK_WORKING_DIRECTORY)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "TestSparkAutomationNullRHISmoke.cmake requires -D${_required}=<value>")
    endif()
endforeach()

if(NOT EXISTS "${SPARK_AUTOMATION}" OR NOT EXISTS "${SPARK_ENGINE}" OR NOT EXISTS "${SPARK_GAME}")
    message(FATAL_ERROR "SparkAutomation, SparkEngine, or SparkGameFPS is missing")
endif()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "SparkAutomation working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()

set(_root "${CMAKE_BINARY_DIR}/spark-automation-nullrhi-smoke")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}")
set(_log "${_root}/runtime.log")
set(_json "${_root}/result.json")
set(_junit "${_root}/result.xml")

# The shipped automation host owns process launch, timeout, captured diagnostics,
# and report generation. -headless is an explicit Windows NullRHI request; the
# bounded frame count keeps this executable smoke independent of a real GPU.
execute_process(
    COMMAND "${SPARK_AUTOMATION}"
        --name spark-automation-nullrhi-smoke
        --executable "${SPARK_ENGINE}"
        --working-dir "${SPARK_WORKING_DIRECTORY}"
        --timeout-ms 90000
        --frames 8
        --expected-exit 0
        --captured-log "${_log}"
        --log-contains "SPARK_MODULE_READY count=1"
        --log-contains "SPARK_HEADLESS_RHI backend=null initialized=1"
        --log-contains "SPARK_HEADLESS_LIFECYCLE initialized=1"
        --json "${_json}"
        --junit "${_junit}"
        --
        -headless
        -game "${SPARK_GAME}"
        -require-game
        -threads 2
        -no-subprocess
    WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 120
    ENCODING UTF-8
)

if(NOT "${_result}" STREQUAL "0")
    message(FATAL_ERROR "SparkAutomation NullRHI smoke exited ${_result}.\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
foreach(_artifact IN ITEMS "${_log}" "${_json}" "${_junit}")
    if(NOT EXISTS "${_artifact}")
        message(FATAL_ERROR "SparkAutomation did not produce expected report: ${_artifact}")
    endif()
endforeach()

file(READ "${_json}" _report)
if(NOT _report MATCHES "\"passed\"[ \\t]*: [ \\t]*true")
    message(FATAL_ERROR "SparkAutomation JSON report did not pass:\n${_report}")
endif()
if(NOT _report MATCHES "\"exitCode\"[ \\t]*: [ \\t]*0")
    message(FATAL_ERROR "SparkAutomation JSON report did not record exitCode=0:\n${_report}")
endif()
if(NOT _report MATCHES "\"frameLimit\"[ \\t]*: [ \\t]*8")
    message(FATAL_ERROR "SparkAutomation JSON report did not record frameLimit=8:\n${_report}")
endif()
file(READ "${_junit}" _junit_report)
if(NOT _junit_report MATCHES "<testsuite name=\"SparkAutomation\" tests=\"1\" failures=\"0\"")
    message(FATAL_ERROR "SparkAutomation JUnit report did not record one passing test:\n${_junit_report}")
endif()

file(READ "${_log}" _log_contents)
string(REPLACE "\r\n" "\n" _log_contents "${_log_contents}")
string(REPLACE "\r" "\n" _log_contents "${_log_contents}")
string(FIND "${_log_contents}" "SPARK_D3D11_DEVICE" _d3d11_position)
if(NOT _d3d11_position EQUAL -1)
    message(FATAL_ERROR "Automation smoke launched a D3D11 device during the NullRHI run:\n${_log_contents}")
endif()
if(NOT _log_contents MATCHES "SPARK_HEADLESS_RHI backend=null initialized=1 frames=[1-9][0-9]* shutdown=1")
    message(FATAL_ERROR "Automation smoke did not produce a completed NullRHI frame record:\n${_log_contents}")
endif()
if(NOT _log_contents MATCHES "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=[1-9][0-9]* fixed=[1-9][0-9]* rendered=0 unloaded=1 faults=0")
    message(FATAL_ERROR "Automation smoke did not produce an exact headless lifecycle record:\n${_log_contents}")
endif()
message(STATUS "SparkAutomation NullRHI executable smoke completed with exact reports and lifecycle evidence")
