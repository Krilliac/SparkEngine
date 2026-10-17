cmake_minimum_required(VERSION 3.25)

set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/RunSparkHeadlessNullRHILifecycle.cmake")

if(SPARK_AUTOMATION_SMOKE_PATH_SELF_TEST)
    function(_spark_assert_rejected_output output trusted name)
        set(SPARK_AUTOMATION_SMOKE_OUTPUT_DIR "${output}")
        set(SPARK_AUTOMATION_SMOKE_BUILD_DIR "${trusted}")
        _spark_validate_output_directory(_accepted _reason)
        if(_accepted)
            message(FATAL_ERROR "Output-path self-test '${name}' unexpectedly passed")
        endif()
    endfunction()
endif()

function(_spark_validate_output_directory out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    if(NOT DEFINED SPARK_AUTOMATION_SMOKE_OUTPUT_DIR OR
       "${SPARK_AUTOMATION_SMOKE_OUTPUT_DIR}" STREQUAL "")
        set(_ok FALSE)
        set(_reason "SPARK_AUTOMATION_SMOKE_OUTPUT_DIR is required")
    elseif(NOT IS_ABSOLUTE "${SPARK_AUTOMATION_SMOKE_OUTPUT_DIR}")
        set(_ok FALSE)
        set(_reason "SPARK_AUTOMATION_SMOKE_OUTPUT_DIR must be absolute")
    elseif(NOT DEFINED SPARK_AUTOMATION_SMOKE_BUILD_DIR OR
           "${SPARK_AUTOMATION_SMOKE_BUILD_DIR}" STREQUAL "")
        set(_ok FALSE)
        set(_reason "SPARK_AUTOMATION_SMOKE_BUILD_DIR is required")
    elseif(NOT IS_ABSOLUTE "${SPARK_AUTOMATION_SMOKE_BUILD_DIR}")
        set(_ok FALSE)
        set(_reason "SPARK_AUTOMATION_SMOKE_BUILD_DIR must be absolute")
    else()
        get_filename_component(_trusted_build "${SPARK_AUTOMATION_SMOKE_BUILD_DIR}" REALPATH)
        get_filename_component(_output "${SPARK_AUTOMATION_SMOKE_OUTPUT_DIR}" REALPATH)
        get_filename_component(_source_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
        get_filename_component(_current_directory "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
        get_filename_component(_output_parent "${_output}" DIRECTORY)
        get_filename_component(_output_leaf "${_output}" NAME)
        if(NOT IS_DIRECTORY "${_trusted_build}")
            set(_ok FALSE)
            set(_reason "trusted build directory is not an existing directory")
        elseif("${_trusted_build}" STREQUAL "${_source_root}" OR
               "${_trusted_build}" STREQUAL "${_current_directory}")
            set(_ok FALSE)
            set(_reason "trusted build directory resolves to a source/cmake directory")
        elseif("${_output}" STREQUAL "${_source_root}" OR
               "${_output}" STREQUAL "${_current_directory}" OR
               "${_output}" STREQUAL "${_trusted_build}")
            set(_ok FALSE)
            set(_reason "output directory resolves to a source, cwd, or trusted-build root")
        elseif(NOT "${_output_parent}" STREQUAL "${_trusted_build}" OR
               NOT "${_output_leaf}" STREQUAL "spark-automation-nullrhi-smoke")
            set(_ok FALSE)
            set(_reason "output must be the dedicated spark-automation-nullrhi-smoke child of the trusted build directory")
        endif()
    endif()
    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_AUTOMATION_SMOKE_PATH_SELF_TEST)
    get_filename_component(_path_test_root "${CMAKE_CURRENT_LIST_DIR}/path-self-test-build" ABSOLUTE)
    _spark_validate_output_directory(_undefined_ok _undefined_reason)
    if(_undefined_ok)
        message(FATAL_ERROR "Output-path self-test 'undefined' unexpectedly passed")
    endif()
    file(MAKE_DIRECTORY "${_path_test_root}")
    set(SPARK_AUTOMATION_SMOKE_OUTPUT_DIR "${_path_test_root}/spark-automation-nullrhi-smoke")
    set(SPARK_AUTOMATION_SMOKE_BUILD_DIR "${_path_test_root}")
    _spark_validate_output_directory(_valid_ok _valid_reason)
    if(NOT _valid_ok)
        message(FATAL_ERROR "Output-path self-test 'valid-shape' failed: ${_valid_reason}")
    endif()
    _spark_assert_rejected_output("${CMAKE_CURRENT_LIST_DIR}/spark-automation-nullrhi-smoke" "${_path_test_root}" wrong-parent)
    _spark_assert_rejected_output("${_path_test_root}" "${_path_test_root}" trusted-root)
    get_filename_component(_source_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
    _spark_assert_rejected_output("${_source_root}/spark-automation-nullrhi-smoke" "${_path_test_root}" source-root)
    file(REMOVE_RECURSE "${_path_test_root}")
    message(STATUS "SparkAutomation output-path rejection self-tests passed")
    return()
endif()

foreach(_required SPARK_AUTOMATION SPARK_ENGINE SPARK_GAME SPARK_WORKING_DIRECTORY
                 SPARK_AUTOMATION_SMOKE_OUTPUT_DIR SPARK_AUTOMATION_SMOKE_BUILD_DIR)
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

_spark_validate_output_directory(_output_ok _output_reason)
if(NOT _output_ok)
    message(FATAL_ERROR "Refusing SparkAutomation smoke output path: ${_output_reason}")
endif()
get_filename_component(_root "${SPARK_AUTOMATION_SMOKE_OUTPUT_DIR}" REALPATH)
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
string(JSON _report_type ERROR_VARIABLE _json_error TYPE "${_report}")
if(NOT "${_json_error}" STREQUAL "NOTFOUND" OR NOT "${_report_type}" STREQUAL "OBJECT")
    message(FATAL_ERROR "SparkAutomation JSON report is not a top-level object: ${_json_error}")
endif()
foreach(_field passed exitCode frameLimit)
    string(JSON _field_type ERROR_VARIABLE _json_error TYPE "${_report}" "${_field}")
    if(NOT "${_json_error}" STREQUAL "NOTFOUND")
        message(FATAL_ERROR "SparkAutomation JSON report is missing ${_field}: ${_json_error}")
    endif()
endforeach()
string(JSON _passed_type TYPE "${_report}" passed)
string(JSON _exit_code_type TYPE "${_report}" exitCode)
string(JSON _frame_limit_type TYPE "${_report}" frameLimit)
if(NOT "${_passed_type}" STREQUAL "BOOLEAN" OR NOT "${_exit_code_type}" STREQUAL "NUMBER" OR
   NOT "${_frame_limit_type}" STREQUAL "NUMBER")
    message(FATAL_ERROR "SparkAutomation JSON report fields have unexpected types: passed=${_passed_type}, exitCode=${_exit_code_type}, frameLimit=${_frame_limit_type}")
endif()
string(JSON _passed GET "${_report}" passed)
string(JSON _exit_code GET "${_report}" exitCode)
string(JSON _frame_limit GET "${_report}" frameLimit)
if(NOT _passed OR NOT "${_exit_code}" STREQUAL "0" OR NOT "${_frame_limit}" STREQUAL "8")
    message(FATAL_ERROR "SparkAutomation JSON report fields failed typed validation: passed=${_passed}, exitCode=${_exit_code}, frameLimit=${_frame_limit}")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_junit_summary "${_root}/junit-summary.json")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../.github/scripts/summarize-test-results.py"
        "${_junit}" --min-tests 1 --json "${_junit_summary}"
    RESULT_VARIABLE _junit_result
    OUTPUT_VARIABLE _junit_stdout
    ERROR_VARIABLE _junit_stderr
)
if(NOT "${_junit_result}" STREQUAL "0")
    message(FATAL_ERROR "Existing JUnit parser rejected SparkAutomation report (${_junit_result}).\nstdout:\n${_junit_stdout}\nstderr:\n${_junit_stderr}")
endif()
file(READ "${_junit_summary}" _junit_stats)
string(JSON _junit_tests GET "${_junit_stats}" tests)
string(JSON _junit_passed GET "${_junit_stats}" passed)
string(JSON _junit_failures GET "${_junit_stats}" failures)
string(JSON _junit_errors GET "${_junit_stats}" errors)
if(NOT "${_junit_tests}" STREQUAL "1" OR NOT "${_junit_passed}" STREQUAL "1" OR
   NOT "${_junit_failures}" STREQUAL "0" OR NOT "${_junit_errors}" STREQUAL "0")
    message(FATAL_ERROR "Existing JUnit parser did not report one clean SparkAutomation testcase: ${_junit_stats}")
endif()

# The summary parser establishes valid XML and outcome counts. This additional
# ElementTree check validates the exact root/testcase identity and rejects any
# failure-like child, without depending on fragile XML substrings.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import sys, xml.etree.ElementTree as ET; root=ET.parse(sys.argv[1]).getroot(); cases=list(root); ok=(root.tag == 'testsuite' and root.attrib.get('name') == 'SparkAutomation' and root.attrib.get('tests') == '1' and root.attrib.get('failures') == '0' and len(cases) == 1 and cases[0].tag == 'testcase' and cases[0].attrib.get('classname') == 'SparkAutomation' and cases[0].attrib.get('name') == 'spark-automation-nullrhi-smoke' and not any(child.tag in {'failure','error','skipped','flakyFailure'} for child in cases[0])); print('strict JUnit shape passed' if ok else 'strict JUnit shape failed', file=sys.stderr if not ok else sys.stdout); raise SystemExit(0 if ok else 1)" "${_junit}"
    RESULT_VARIABLE _junit_shape_result
    OUTPUT_VARIABLE _junit_shape_stdout
    ERROR_VARIABLE _junit_shape_stderr
)
if(NOT "${_junit_shape_result}" STREQUAL "0")
    message(FATAL_ERROR "SparkAutomation JUnit report failed strict structural validation.\nstdout:\n${_junit_shape_stdout}\nstderr:\n${_junit_shape_stderr}")
endif()

file(READ "${_log}" _log_contents)
string(REPLACE "\r\n" "\n" _log_contents "${_log_contents}")
string(REPLACE "\r" "\n" _log_contents "${_log_contents}")
string(FIND "${_log_contents}" "SPARK_D3D11_DEVICE" _d3d11_position)
if(NOT _d3d11_position EQUAL -1)
    message(FATAL_ERROR "Automation smoke launched a D3D11 device during the NullRHI run:\n${_log_contents}")
endif()
_spark_validate_headless_nullrhi_result(0 "${_log_contents}" "" _markers_ok _markers_reason)
if(NOT _markers_ok)
    message(FATAL_ERROR "Automation smoke did not produce strict NullRHI lifecycle evidence: ${_markers_reason}\n${_log_contents}")
endif()
message(STATUS "SparkAutomation NullRHI executable smoke completed with exact reports and lifecycle evidence")
