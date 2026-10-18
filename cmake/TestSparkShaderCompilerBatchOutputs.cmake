# SEC2: SparkShaderCompiler -batch must never let two sources write one artifact.
#
# Before the fix, -batch -o flattened every source to <out>/<stem>.cso, so
# a/BasicVS.hlsl and b/BasicVS.hlsl both wrote <out>/BasicVS.cso, and BasicVS.hlsl
# plus BasicVS.vs in one directory both wrote BasicVS.cso. The later compile
# silently replaced the earlier artifact and the run still exited 0.
#
# Case 1: -o preserves the relative layout (two artifacts, exit 0).
# Case 2: a same-directory stem collision fails before compiling anything.
# Case 3: a symlinked (or junctioned) source keeps its artifact under -o.

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

# Case 3: a linked source must not steer its artifact out of -o. The first layout fix
# built the relative path with fs::relative(), which resolves links (weakly_canonical):
# <batch>/x.hlsl -> <elsewhere>/deep/Evil.hlsl became ../elsewhere/deep/Evil.hlsl and the
# compile created <elsewhere>/deep/Evil.cso. The relative path is now lexical, so the
# artifact is <out>/x.cso. A directory junction is tried as well (created without
# privileges); whether the iterator descends into it is up to the STL, but if it does,
# its artifact must also stay under <out>.
set(_link_root "${SPARK_SHADER_OUTPUT_DIR}/link-src")
set(_link_out "${SPARK_SHADER_OUTPUT_DIR}/link-out")
set(_elsewhere "${SPARK_SHADER_OUTPUT_DIR}/elsewhere")
file(MAKE_DIRECTORY "${_link_root}/plain" "${_elsewhere}/deep" "${_elsewhere}/lib")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_link_root}/plain/BasicVS.hlsl")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_elsewhere}/deep/Evil.hlsl")
file(COPY_FILE "${SPARK_SHADER_FIXTURE}" "${_elsewhere}/lib/Junction.hlsl")
file(CREATE_LINK "${_elsewhere}/deep/Evil.hlsl" "${_link_root}/x.hlsl" RESULT _symlink_result SYMBOLIC)
file(TO_NATIVE_PATH "${_link_root}/lib" _junction_native)
file(TO_NATIVE_PATH "${_elsewhere}/lib" _junction_target_native)
execute_process(
    COMMAND cmd /c mklink /J "${_junction_native}" "${_junction_target_native}"
    RESULT_VARIABLE _junction_result
    OUTPUT_QUIET ERROR_QUIET)
if(NOT _symlink_result EQUAL 0 AND NOT _junction_result EQUAL 0)
    message(FATAL_ERROR "Could not create a file symlink (${_symlink_result}) or a junction (${_junction_result}); "
                        "the link-escape case would check nothing")
endif()

execute_process(
    COMMAND "${SPARK_SHADER_COMPILER}" -batch "${_link_root}" -backend d3d11 -o "${_link_out}"
    WORKING_DIRECTORY "${SPARK_SHADER_OUTPUT_DIR}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
file(GLOB_RECURSE _escaped LIST_DIRECTORIES false "${_elsewhere}/*.cso" "${_elsewhere}/*.spv")
if(_escaped)
    message(FATAL_ERROR "Linked batch source wrote outside -o: ${_escaped}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Linked batch failed (${_result})\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(NOT EXISTS "${_link_out}/plain/BasicVS.cso")
    message(FATAL_ERROR "Linked batch did not compile the plain source\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(_symlink_result EQUAL 0)
    if(NOT EXISTS "${_link_out}/x.cso")
        message(FATAL_ERROR "Symlinked source was not compiled to ${_link_out}/x.cso\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
else()
    message(STATUS "File symlink unavailable (${_symlink_result}); junction case only")
endif()
if(_junction_result EQUAL 0)
    execute_process(COMMAND cmd /c rmdir "${_junction_native}" OUTPUT_QUIET ERROR_QUIET)
endif()

message(STATUS "SparkShaderCompiler batch outputs: layout preserved, collisions rejected, links contained")
