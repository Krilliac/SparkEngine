# Execute the shipped SparkShaderCompiler binary against a checked-in HLSL
# fixture. This is deliberately an executable-level test: --help/--version
# probes are insufficient to prove the compiler and output path work.

if(NOT DEFINED SPARK_SHADER_COMPILER OR SPARK_SHADER_COMPILER STREQUAL "")
    message(FATAL_ERROR "SparkShaderCompiler smoke requires SPARK_SHADER_COMPILER")
endif()
if(NOT DEFINED SPARK_SHADER_FIXTURE OR SPARK_SHADER_FIXTURE STREQUAL "")
    message(FATAL_ERROR "SparkShaderCompiler smoke requires SPARK_SHADER_FIXTURE")
endif()
if(NOT DEFINED SPARK_SHADER_OUTPUT_DIR OR SPARK_SHADER_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "SparkShaderCompiler smoke requires SPARK_SHADER_OUTPUT_DIR")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "SparkShaderCompiler executable smoke requires the Windows D3D compiler backend")
endif()
if(NOT EXISTS "${SPARK_SHADER_COMPILER}" OR IS_DIRECTORY "${SPARK_SHADER_COMPILER}")
    message(FATAL_ERROR "SparkShaderCompiler binary is missing: ${SPARK_SHADER_COMPILER}")
endif()
if(NOT EXISTS "${SPARK_SHADER_FIXTURE}" OR IS_DIRECTORY "${SPARK_SHADER_FIXTURE}")
    message(FATAL_ERROR "SparkShaderCompiler fixture is missing: ${SPARK_SHADER_FIXTURE}")
endif()

file(REMOVE_RECURSE "${SPARK_SHADER_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${SPARK_SHADER_OUTPUT_DIR}")
set(_output "${SPARK_SHADER_OUTPUT_DIR}/BasicVS.cso")

# Run from the binary tree rather than the source tree. The fixture is passed
# by absolute path so source-relative working-directory assumptions fail here.
execute_process(
    COMMAND "${SPARK_SHADER_COMPILER}" "${SPARK_SHADER_FIXTURE}"
            -stage vertex -backend d3d11 -entry main -o "${_output}"
    WORKING_DIRECTORY "${SPARK_SHADER_OUTPUT_DIR}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "SparkShaderCompiler failed (${_result})\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(NOT EXISTS "${_output}" OR IS_DIRECTORY "${_output}")
    message(FATAL_ERROR "SparkShaderCompiler produced no output artifact: ${_output}")
endif()
file(SIZE "${_output}" _size)
if(_size LESS 64)
    message(FATAL_ERROR "SparkShaderCompiler output is implausibly small (${_size} bytes): ${_output}")
endif()

message(STATUS "SparkShaderCompiler executable smoke passed (${_size} byte DXBC artifact)")
