cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SPARK_WORKER OR NOT EXISTS "${SPARK_WORKER}")
    message(FATAL_ERROR "SPARK_WORKER must name the built SparkWorker executable")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT must name a disposable test directory")
endif()

function(require_exit expected label)
    if(NOT worker_result EQUAL expected)
        message(FATAL_ERROR
            "${label}: expected exit ${expected}, got ${worker_result}\n"
            "stdout: ${worker_stdout}\n"
            "stderr: ${worker_stderr}")
    endif()
endfunction()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

execute_process(
    COMMAND "${SPARK_WORKER}" --help
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(0 "help")
if(NOT worker_stderr MATCHES "Usage: SparkWorker")
    message(FATAL_ERROR "help: expected usage text on stderr, got: ${worker_stderr}")
endif()

execute_process(
    COMMAND "${SPARK_WORKER}" --source missing --output missing --sha256 short
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(2 "malformed digest")
if(NOT worker_stderr MATCHES "64-character SHA-256")
    message(FATAL_ERROR "malformed digest: expected validation diagnostic, got: ${worker_stderr}")
endif()

set(source "${TEST_ROOT}/source.bin")
set(output "${TEST_ROOT}/cooked/output.bin")
file(WRITE "${source}" "SparkWorker CLI contract payload\n")
file(SHA256 "${source}" digest)
string(REPEAT "0" 64 wrong_digest)

execute_process(
    COMMAND "${SPARK_WORKER}" --source "${source}" --output "${output}" --sha256 "${wrong_digest}"
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(1 "digest mismatch")
if(EXISTS "${output}")
    message(FATAL_ERROR "digest mismatch: output must not be created")
endif()

execute_process(
    COMMAND "${SPARK_WORKER}" --source "${source}" --output "${output}" --sha256 "${digest}" --dry-run
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(0 "dry run")
if(EXISTS "${output}")
    message(FATAL_ERROR "dry run: output must not be created")
endif()
if(NOT worker_stdout MATCHES "^updated ")
    message(FATAL_ERROR "dry run: expected an update preview, got: ${worker_stdout}")
endif()

execute_process(
    COMMAND "${SPARK_WORKER}" --source "${source}" --output "${output}" --sha256 "${digest}"
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(0 "initial cook")
if(NOT EXISTS "${output}")
    message(FATAL_ERROR "initial cook: output was not created")
endif()
file(SHA256 "${output}" output_digest)
if(NOT output_digest STREQUAL digest)
    message(FATAL_ERROR "initial cook: output digest ${output_digest} does not match ${digest}")
endif()
if(NOT worker_stdout MATCHES "^updated ")
    message(FATAL_ERROR "initial cook: expected updated result, got: ${worker_stdout}")
endif()

execute_process(
    COMMAND "${SPARK_WORKER}" --source "${source}" --output "${output}" --sha256 "${digest}"
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(0 "unchanged cook")
if(NOT worker_stdout MATCHES "^unchanged ")
    message(FATAL_ERROR "unchanged cook: expected unchanged result, got: ${worker_stdout}")
endif()

set(relative_source "${TEST_ROOT}/relative-source.bin")
set(relative_output "${TEST_ROOT}/relative-output.bin")
file(WRITE "${relative_source}" "SparkWorker relative output payload\n")
file(SHA256 "${relative_source}" relative_digest)
execute_process(
    COMMAND "${SPARK_WORKER}"
            --source "relative-source.bin"
            --output "relative-output.bin"
            --sha256 "${relative_digest}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE worker_result
    OUTPUT_VARIABLE worker_stdout
    ERROR_VARIABLE worker_stderr
)
require_exit(0 "working-directory-relative output")
if(NOT EXISTS "${relative_output}")
    message(FATAL_ERROR "working-directory-relative output: output was not created")
endif()
file(SHA256 "${relative_output}" relative_output_digest)
if(NOT relative_output_digest STREQUAL relative_digest)
    message(FATAL_ERROR
        "working-directory-relative output: output digest ${relative_output_digest} does not match ${relative_digest}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
