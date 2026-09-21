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
    file(SIZE "${_source}/${_relative}" _source_size)
    file(SIZE "${_output}/${_relative}" _output_size)
    if(NOT _source_size EQUAL _output_size)
        message(FATAL_ERROR "Cooked asset size differs from source: ${_relative}")
    endif()
    if(_relative STREQUAL "alpha.txt")
        set(_alpha_sha "${_source_sha}")
        set(_alpha_size "${_source_size}")
    else()
        set(_beta_sha "${_source_sha}")
        set(_beta_size "${_source_size}")
    endif()
endforeach()
if(NOT EXISTS "${_manifest}" OR IS_DIRECTORY "${_manifest}")
    message(FATAL_ERROR "SparkCooker did not publish its manifest: ${_manifest}")
endif()
file(READ "${_manifest}" _manifest_text)
string(REGEX MATCHALL "\"schemaVersion\"" _manifest_schema_fields "${_manifest_text}")
list(LENGTH _manifest_schema_fields _manifest_schema_count)
string(FIND "${_manifest_text}" "\"schemaVersion\": 1" _manifest_schema)
string(REGEX MATCHALL "\"path\"" _manifest_paths "${_manifest_text}")
list(LENGTH _manifest_paths _manifest_path_count)
string(REGEX MATCHALL "\"sha256\"" _manifest_hash_fields "${_manifest_text}")
list(LENGTH _manifest_hash_fields _manifest_hash_count)
string(REGEX MATCHALL "\"size\"" _manifest_size_fields "${_manifest_text}")
list(LENGTH _manifest_size_fields _manifest_size_count)
string(FIND "${_manifest_text}" "\"path\": \"alpha.txt\", \"sha256\": \"${_alpha_sha}\", \"size\": ${_alpha_size}" _alpha_record)
string(FIND "${_manifest_text}" "\"path\": \"nested/beta.txt\", \"sha256\": \"${_beta_sha}\", \"size\": ${_beta_size}" _beta_record)
if(NOT _manifest_schema_count EQUAL 1 OR _manifest_schema EQUAL -1 OR
   NOT _manifest_path_count EQUAL 2 OR NOT _manifest_hash_count EQUAL 2 OR
   NOT _manifest_size_count EQUAL 2 OR _alpha_record EQUAL -1 OR _beta_record EQUAL -1)
    message(FATAL_ERROR "SparkCooker manifest is incomplete or malformed:\n${_manifest_text}")
endif()

# The fixture bytes and record order above are fixed. This is the independently
# recorded SHA-256 of AssetCooker's NUL-delimited HashRecords stream for those
# exact two records; binding both the manifest and CLI summary prevents a field
# that is merely present (but unrelated to the records) from passing.
set(_aggregate_sha "5ca548929390991a7c6bf65d953f7a50a648fc4cd933dfa368eb7402942683f1")
string(REGEX MATCHALL "\"manifestSha256\"" _manifest_digest_fields "${_manifest_text}")
list(LENGTH _manifest_digest_fields _manifest_digest_count)
string(FIND "${_manifest_text}" "\"manifestSha256\": \"${_aggregate_sha}\"" _manifest_digest)
string(FIND "${_first_stdout}" "manifest-sha256 ${_aggregate_sha}" _stdout_digest)
if(NOT _manifest_digest_count EQUAL 1 OR _manifest_digest EQUAL -1 OR _stdout_digest EQUAL -1)
    message(FATAL_ERROR "SparkCooker manifest aggregate digest is incorrect:\n${_manifest_text}")
endif()

# Snapshot the first generation as bytes. The second invocation must preserve
# both asset and manifest contents, not merely print an unchanged summary.
file(READ "${_output}/alpha.txt" _first_alpha_hex HEX)
file(READ "${_output}/nested/beta.txt" _first_beta_hex HEX)
file(READ "${_manifest}" _first_manifest_hex HEX)

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
file(READ "${_output}/alpha.txt" _second_alpha_hex HEX)
file(READ "${_output}/nested/beta.txt" _second_beta_hex HEX)
file(READ "${_manifest}" _second_manifest_hex HEX)
if(NOT _first_alpha_hex STREQUAL _second_alpha_hex OR
   NOT _first_beta_hex STREQUAL _second_beta_hex OR
   NOT _first_manifest_hex STREQUAL _second_manifest_hex)
    message(FATAL_ERROR "SparkCooker changed published bytes during the unchanged second cook")
endif()

message(STATUS "SparkCooker executable smoke passed (2 assets cooked and revalidated)")
