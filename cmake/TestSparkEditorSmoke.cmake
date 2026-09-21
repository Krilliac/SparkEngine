# Execute the shipped SparkEditor against an isolated project and require a
# bounded, clean startup/project-load/shutdown cycle.  A frame-limit-only run
# is not sufficient: this must prove the executable consumed a project file.

if(NOT DEFINED SPARK_EDITOR OR SPARK_EDITOR STREQUAL "")
    message(FATAL_ERROR "SparkEditor smoke requires SPARK_EDITOR")
endif()
if(NOT DEFINED SPARK_EDITOR_WORK_DIR OR SPARK_EDITOR_WORK_DIR STREQUAL "")
    message(FATAL_ERROR "SparkEditor smoke requires SPARK_EDITOR_WORK_DIR")
endif()
if(NOT EXISTS "${SPARK_EDITOR}" OR IS_DIRECTORY "${SPARK_EDITOR}")
    message(FATAL_ERROR "SparkEditor binary is missing: ${SPARK_EDITOR}")
endif()

file(REMOVE_RECURSE "${SPARK_EDITOR_WORK_DIR}")
file(MAKE_DIRECTORY "${SPARK_EDITOR_WORK_DIR}/Assets")
file(MAKE_DIRECTORY "${SPARK_EDITOR_WORK_DIR}/Scenes")

# The editor's project loader accepts a minimal project document.  An empty
# scene list deliberately exercises the safe new-scene fallback while keeping
# the fixture independent of repository assets and modules.
set(_project "${SPARK_EDITOR_WORK_DIR}/EditorSmoke.sparkproject")
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

set(_output "${SPARK_EDITOR_WORK_DIR}/editor-smoke-output.txt")
set(_result_file "${SPARK_EDITOR_WORK_DIR}/editor-smoke-result.json")
execute_process(
    COMMAND "${SPARK_EDITOR}" --test-mode --test-frames 3 --project "${_project}" --smoke-result "${_result_file}"
    WORKING_DIRECTORY "${SPARK_EDITOR_WORK_DIR}"
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
