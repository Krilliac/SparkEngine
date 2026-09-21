# Execute the shipped SparkEditor against an isolated project and require a
# bounded, clean startup/project-load/shutdown cycle.  A frame-limit-only run
# is not sufficient: this must prove the executable consumed a project file.

if(NOT DEFINED SPARK_EDITOR OR SPARK_EDITOR STREQUAL "")
    message(FATAL_ERROR "SparkEditor smoke requires SPARK_EDITOR")
endif()
if(NOT DEFINED SPARK_EDITOR_BUILD_DIR OR SPARK_EDITOR_BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "SparkEditor smoke requires SPARK_EDITOR_BUILD_DIR")
endif()
if(NOT DEFINED SPARK_EDITOR_WORK_DIR OR SPARK_EDITOR_WORK_DIR STREQUAL "")
    message(FATAL_ERROR "SparkEditor smoke requires SPARK_EDITOR_WORK_DIR")
endif()
if(NOT EXISTS "${SPARK_EDITOR}" OR IS_DIRECTORY "${SPARK_EDITOR}")
    message(FATAL_ERROR "SparkEditor binary is missing: ${SPARK_EDITOR}")
endif()
if(NOT EXISTS "${SPARK_EDITOR_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "SparkEditor smoke build directory is not a configured CMake tree: ${SPARK_EDITOR_BUILD_DIR}")
endif()

set(_build_dir "${SPARK_EDITOR_BUILD_DIR}")
set(_work_dir "${SPARK_EDITOR_WORK_DIR}")
cmake_path(ABSOLUTE_PATH _build_dir NORMALIZE)
cmake_path(ABSOLUTE_PATH _work_dir NORMALIZE)
file(REAL_PATH "${_build_dir}" _canonical_build_dir)
set(_owned_root "${_canonical_build_dir}/editor-executable-smoke")
if(EXISTS "${_owned_root}")
    file(REAL_PATH "${_owned_root}" _canonical_owned_root)
else()
    set(_canonical_owned_root "${_owned_root}")
endif()
if(EXISTS "${_work_dir}")
    file(REAL_PATH "${_work_dir}" _canonical_work_dir)
else()
    get_filename_component(_work_parent "${_work_dir}" DIRECTORY)
    if(NOT IS_DIRECTORY "${_work_parent}")
        message(FATAL_ERROR "SparkEditor smoke work directory parent is not a directory: ${_work_parent}")
    endif()
    file(REAL_PATH "${_work_parent}" _canonical_work_parent)
    get_filename_component(_work_leaf "${_work_dir}" NAME)
    set(_canonical_work_dir "${_canonical_work_parent}/${_work_leaf}")
endif()
cmake_path(IS_PREFIX _canonical_owned_root "${_canonical_work_dir}" NORMALIZE _work_is_owned)
if(NOT _work_is_owned OR _canonical_work_dir STREQUAL _canonical_owned_root)
    message(FATAL_ERROR
        "Refusing to erase SparkEditor smoke path outside the trusted build output child: ${_work_dir}\n"
        "Canonical expected root: ${_canonical_owned_root}")
endif()

file(REMOVE_RECURSE "${_canonical_work_dir}")
file(MAKE_DIRECTORY "${_canonical_work_dir}/Assets")
file(MAKE_DIRECTORY "${_canonical_work_dir}/Scenes")

# The editor's project loader accepts a minimal project document.  An empty
# scene list deliberately exercises the safe new-scene fallback while keeping
# the fixture independent of repository assets and modules.
set(_project "${_canonical_work_dir}/EditorSmoke.sparkproject")
file(WRITE "${_project}" [=[{
  "projectFileVersion": 1,
  "name": "EditorSmoke",
  "version": "1.0.0",
  "description": "CI executable startup smoke",
  "engineVersion": "1.0.0",
  "template": "empty",
  "defaultScene": "",
  "lastOpenedScene": "",
  "createdTime": 0,
  "lastModified": 0,
  "modules": [],
  "scenes": []
}
]=])

set(_output "${_canonical_work_dir}/editor-smoke-output.txt")
set(_result_file "${_canonical_work_dir}/editor-smoke-result.json")
execute_process(
    COMMAND "${SPARK_EDITOR}" --test-mode --test-frames 3 --project "${_project}" --smoke-result "${_result_file}"
    WORKING_DIRECTORY "${_canonical_work_dir}"
    TIMEOUT 30
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
file(WRITE "${_output}" "${_stdout}\n${_stderr}")

if(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "SparkEditor smoke exited with ${_result}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()

if(NOT EXISTS "${_result_file}" OR IS_DIRECTORY "${_result_file}")
    message(FATAL_ERROR "SparkEditor smoke did not publish structured result: ${_result_file}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
file(READ "${_result_file}" _result_json)
foreach(_marker IN ITEMS
        "\"schema\": 1"
        "\"status\": \"passed\""
        "\"projectLoaded\": true"
        "\"runResult\": 0")
    string(FIND "${_result_json}" "${_marker}" _marker_offset)
    if(_marker_offset EQUAL -1)
        message(FATAL_ERROR
            "SparkEditor smoke result is missing required marker '${_marker}'\nresult:\n${_result_json}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endforeach()

message(STATUS "SparkEditor executable smoke passed (project load, 3-frame run, clean shutdown)")
