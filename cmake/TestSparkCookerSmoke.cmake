# Execute the shipped SparkCooker binary against a small, owned source tree.
# This is intentionally an executable-level test: --help/--version probes do
# not prove asset publication, manifest generation, or incremental stability.

if(NOT DEFINED SPARK_COOKER OR SPARK_COOKER STREQUAL "")
    message(FATAL_ERROR "SparkCooker smoke requires SPARK_COOKER")
endif()
if(NOT DEFINED SPARK_COOKER_WORK_DIR OR SPARK_COOKER_WORK_DIR STREQUAL "")
    message(FATAL_ERROR "SparkCooker smoke requires SPARK_COOKER_WORK_DIR")
endif()
if(NOT EXISTS "${SPARK_COOKER}" OR IS_DIRECTORY "${SPARK_COOKER}")
    message(FATAL_ERROR "SparkCooker binary is missing: ${SPARK_COOKER}")
endif()

file(REMOVE_RECURSE "${SPARK_COOKER_WORK_DIR}")
file(MAKE_DIRECTORY "${SPARK_COOKER_WORK_DIR}/source/nested")
file(WRITE "${SPARK_COOKER_WORK_DIR}/source/alpha.txt" "alpha release smoke\n")
file(WRITE "${SPARK_COOKER_WORK_DIR}/source/nested/beta.txt" "nested cooker payload\n")
set(_source "${SPARK_COOKER_WORK_DIR}/source")
set(_output "${SPARK_COOKER_WORK_DIR}/output")
set(_manifest "${_output}/spark-cook-manifest.json")

execute_process(
    COMMAND "${SPARK_COOKER}" --source "${_source}" --output "${_output}"
    WORKING_DIRECTORY "${SPARK_COOKER_WORK_DIR}"
    RESULT_VARIABLE _first_result
    OUTPUT_VARIABLE _first_stdout
    ERROR_VARIABLE _first_stderr)
if(NOT _first_result EQUAL 0)
    message(FATAL_ERROR
        "SparkCooker first cook failed (${_first_result})\nstdout:\n${_first_stdout}\nstderr:\n${_first_stderr}")
endif()
string(FIND "${_first_stdout}" "2 asset(s), 2 updated, 0 unchanged" _first_summary)
if(_first_summary EQUAL -1)
    message(FATAL_ERROR "SparkCooker did not perform the expected first cook:\n${_first_stdout}")
endif()
foreach(_relative IN ITEMS alpha.txt nested/beta.txt)
    if(NOT EXISTS "${_output}/${_relative}" OR IS_DIRECTORY "${_output}/${_relative}")
        message(FATAL_ERROR "SparkCooker did not publish cooked asset: ${_relative}")
    endif()
    file(SIZE "${_output}/${_relative}" _size)
    if(_size LESS 2)
        message(FATAL_ERROR "SparkCooker published an implausibly small asset: ${_relative}")
    endif()
    file(SHA256 "${_source}/${_relative}" _source_sha)
    file(SHA256 "${_output}/${_relative}" _output_sha)
    if(NOT _source_sha STREQUAL _output_sha)
        message(FATAL_ERROR "Cooked asset bytes differ from source: ${_relative}")
    endif()
endforeach()
if(NOT EXISTS "${_manifest}" OR IS_DIRECTORY "${_manifest}")
    message(FATAL_ERROR "SparkCooker did not publish its manifest: ${_manifest}")
endif()
file(READ "${_manifest}" _manifest_text)
string(REGEX MATCHALL "\"path\"" _manifest_paths "${_manifest_text}")
list(LENGTH _manifest_paths _manifest_path_count)
string(FIND "${_manifest_text}" "\"path\": \"alpha.txt\"" _alpha_path)
string(FIND "${_manifest_text}" "\"path\": \"nested/beta.txt\"" _beta_path)
if(NOT _manifest_path_count EQUAL 2 OR _alpha_path EQUAL -1 OR _beta_path EQUAL -1)
    message(FATAL_ERROR "SparkCooker manifest is incomplete or malformed:\n${_manifest_text}")
endif()

# A second real invocation must observe the published generation and remain
# stable. This rejects a smoke that only creates files or ignores existing data.
execute_process(
    COMMAND "${SPARK_COOKER}" --source "${_source}" --output "${_output}"
    WORKING_DIRECTORY "${SPARK_COOKER_WORK_DIR}"
    RESULT_VARIABLE _second_result
    OUTPUT_VARIABLE _second_stdout
    ERROR_VARIABLE _second_stderr)
if(NOT _second_result EQUAL 0)
    message(FATAL_ERROR
        "SparkCooker second cook failed (${_second_result})\nstdout:\n${_second_stdout}\nstderr:\n${_second_stderr}")
endif()
string(FIND "${_second_stdout}" "2 asset(s), 0 updated, 2 unchanged" _second_summary)
if(_second_summary EQUAL -1)
    message(FATAL_ERROR "SparkCooker did not report a stable second cook:\n${_second_stdout}")
endif()

message(STATUS "SparkCooker executable smoke passed (2 assets cooked and revalidated)")
