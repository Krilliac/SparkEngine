# SEC2: SparkShaderCompiler -batch must never let two sources write one artifact.
#
# Before the fix, -batch -o flattened every source to <out>/<stem>.cso, so
# a/BasicVS.hlsl and b/BasicVS.hlsl both wrote <out>/BasicVS.cso, and BasicVS.hlsl
# plus BasicVS.vs in one directory both wrote BasicVS.cso. The later compile
# silently replaced the earlier artifact and the run still exited 0.
#
# Case 1: -o preserves the relative layout (two artifacts, exit 0).
# Case 2: a same-directory stem collision fails before compiling anything.

foreach(_required SPARK_SHADER_COMPILER SPARK_SHADER_FIXTURE SPARK_SHADER_OUTPUT_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "SparkShaderCompiler batch test requires ${_required}")
    endif()
endforeach()
if(NOT WIN32)
    message(FATAL_ERROR "SparkShaderCompiler batch test requires the Windows D3D compiler backend")
endif()
if(NOT EXISTS "${SPARK_SHADER_COMPILER}" OR IS_DIRECTORY "${SPARK_SHADER_COMPILER}")
    message(FATAL_ERROR "SparkShaderCompiler binary is missing: ${SPARK_SHADER_COMPILER}")
endif()
if(NOT EXISTS "${SPARK_SHADER_FIXTURE}" OR IS_DIRECTORY "${SPARK_SHADER_FIXTURE}")
    message(FATAL_ERROR "SparkShaderCompiler fixture is missing: ${SPARK_SHADER_FIXTURE}")
endif()

file(REMOVE_RECURSE "${SPARK_SHADER_OUTPUT_DIR}")
set(_nested_root "${SPARK_SHADER_OUTPUT_DIR}/nested-src")
set(_nested_out "${SPARK_SHADER_OUTPUT_DIR}/nested-out")
set(_collide_root "${SPARK_SHADER_OUTPUT_DIR}/collide-src")
file(MAKE_DIRECTORY "${_nested_root}/a" "${_nested_root}/b" "${_collide_root}")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_nested_root}/a/BasicVS.hlsl")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_nested_root}/b/BasicVS.hlsl")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_collide_root}/BasicVS.hlsl")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_collide_root}/BasicVS.vs")

# Case 1: same basename in two subdirectories.
execute_process(
    COMMAND "${SPARK_SHADER_COMPILER}" -batch "${_nested_root}" -backend d3d11 -o "${_nested_out}"
    WORKING_DIRECTORY "${SPARK_SHADER_OUTPUT_DIR}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Nested batch failed (${_result})\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
foreach(_artifact "${_nested_out}/a/BasicVS.cso" "${_nested_out}/b/BasicVS.cso")
    if(NOT EXISTS "${_artifact}" OR IS_DIRECTORY "${_artifact}")
        message(FATAL_ERROR "Nested batch did not keep the source layout; missing ${_artifact}\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endforeach()
if(EXISTS "${_nested_out}/BasicVS.cso")
    message(FATAL_ERROR "Nested batch still flattened output to ${_nested_out}/BasicVS.cso")
endif()

# Case 2: BasicVS.hlsl and BasicVS.vs in one directory both map to BasicVS.cso.
execute_process(
    COMMAND "${SPARK_SHADER_COMPILER}" -batch "${_collide_root}" -backend d3d11
    WORKING_DIRECTORY "${SPARK_SHADER_OUTPUT_DIR}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(_result EQUAL 0)
    message(FATAL_ERROR "Colliding batch exited 0; one artifact silently replaced the other\n"
                        "stdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
string(FIND "${_stderr}" "both compile to" _collision_reported)
if(_collision_reported EQUAL -1)
    message(FATAL_ERROR "Colliding batch failed without naming the collision (${_result})\n"
                        "stdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(EXISTS "${_collide_root}/BasicVS.cso")
    message(FATAL_ERROR "Colliding batch wrote ${_collide_root}/BasicVS.cso before rejecting the plan")
endif()

message(STATUS "SparkShaderCompiler batch outputs: layout preserved and collisions rejected")
