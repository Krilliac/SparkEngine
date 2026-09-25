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

# Linux registers the smoke with SPARK_EDITOR_REQUIRE_XVFB=ON: every editor run
# gets its own Xvfb server (llvmpipe), so the result never depends on the
# caller's DISPLAY. A missing xvfb-run fails the test instead of skipping it.
set(_launcher)
if(SPARK_EDITOR_REQUIRE_XVFB)
    if(NOT DEFINED SPARK_EDITOR_XVFB_RUN OR NOT EXISTS "${SPARK_EDITOR_XVFB_RUN}"
       OR IS_DIRECTORY "${SPARK_EDITOR_XVFB_RUN}")
        message(FATAL_ERROR
            "SparkEditor smoke requires xvfb-run on this platform, but it was not found at configure time "
            "('${SPARK_EDITOR_XVFB_RUN}'). Install xvfb and reconfigure.")
    endif()
    set(_launcher "${SPARK_EDITOR_XVFB_RUN}" -a -s "-screen 0 1280x720x24")
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
    # The dedicated root is the only directory this test is allowed to create.
    # Creating it here makes a clean checkout/configuration a first-class path.
    file(MAKE_DIRECTORY "${_owned_root}")
    file(REAL_PATH "${_owned_root}" _canonical_owned_root)
endif()
cmake_path(IS_PREFIX _canonical_build_dir "${_canonical_owned_root}" NORMALIZE _owned_root_is_bounded)
if(NOT _owned_root_is_bounded OR _canonical_owned_root STREQUAL _canonical_build_dir)
    message(FATAL_ERROR
        "Refusing to use the editor smoke root outside the configured build tree: ${_canonical_owned_root}")
endif()

get_filename_component(_work_parent "${_work_dir}" DIRECTORY)
get_filename_component(_work_leaf "${_work_dir}" NAME)
if(NOT _work_leaf OR _work_leaf STREQUAL "." OR _work_leaf STREQUAL "..")
    message(FATAL_ERROR "SparkEditor smoke work path must name a configuration child: ${_work_dir}")
endif()
if(EXISTS "${_work_dir}")
    file(REAL_PATH "${_work_dir}" _canonical_work_dir)
else()
    # The parent now exists because the trusted smoke root was created above.
    file(REAL_PATH "${_work_parent}" _canonical_work_parent)
    set(_canonical_work_dir "${_canonical_work_parent}/${_work_leaf}")
endif()
file(REAL_PATH "${_canonical_owned_root}" _canonical_owned_root)
cmake_path(IS_PREFIX _canonical_owned_root "${_canonical_work_dir}" NORMALIZE _work_is_owned)
get_filename_component(_canonical_work_parent "${_canonical_work_dir}" DIRECTORY)
if(NOT _work_is_owned OR NOT _canonical_work_parent STREQUAL _canonical_owned_root)
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

# Keep the missing-scene case independent of the project whose scene is saved
# and reopened below. Startup must load this project before the bad scene is requested.
set(_missing_work_dir "${_canonical_work_dir}/missing-scene-case")
file(MAKE_DIRECTORY "${_missing_work_dir}/Assets" "${_missing_work_dir}/Scenes")
set(_missing_project "${_missing_work_dir}/EditorSmoke.sparkproject")
file(COPY_FILE "${_project}" "${_missing_project}")

set(_output "${_canonical_work_dir}/editor-smoke-output.txt")
set(_result_file "${_canonical_work_dir}/editor-smoke-result.json")
execute_process(
    COMMAND ${_launcher} "${SPARK_EDITOR}" --test-mode --test-frames 3 --project "${_project}" --smoke-result "${_result_file}"
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

# Exercise the explicit scene CLI path through the executable. A successful
# frame run must not conceal a failed --open-scene request.
set(_saved_scene "${_canonical_work_dir}/Scenes/EditorRoundTrip.sparkscene")
execute_process(
    COMMAND ${_launcher} "${SPARK_EDITOR}" --test-mode --project "${_project}"
        --save-scene "Scenes/EditorRoundTrip.sparkscene"
    WORKING_DIRECTORY "${_canonical_work_dir}"
    TIMEOUT 30
    RESULT_VARIABLE _save_result)
if(NOT _save_result EQUAL 0 OR NOT EXISTS "${_saved_scene}")
    message(FATAL_ERROR "SparkEditor could not save the round-trip scene (exit ${_save_result})")
endif()

set(_reopen_result_file "${_canonical_work_dir}/editor-reopen-result.json")
execute_process(
    COMMAND ${_launcher} "${SPARK_EDITOR}" --test-mode --test-frames 3 --project "${_project}"
        --open-scene "Scenes/EditorRoundTrip.sparkscene" --smoke-result "${_reopen_result_file}"
    WORKING_DIRECTORY "${_canonical_work_dir}"
    TIMEOUT 30
    RESULT_VARIABLE _reopen_result)
if(NOT _reopen_result EQUAL 0 OR NOT EXISTS "${_reopen_result_file}")
    message(FATAL_ERROR "SparkEditor could not reopen the saved scene (exit ${_reopen_result})")
endif()
file(READ "${_reopen_result_file}" _reopen_json)
if(NOT _reopen_json MATCHES "\"status\": \"passed\"")
    message(FATAL_ERROR "SparkEditor did not report a successful scene reopen: ${_reopen_json}")
endif()

set(_missing_result_file "${_missing_work_dir}/editor-missing-scene-result.json")
execute_process(
    COMMAND ${_launcher} "${SPARK_EDITOR}" --test-mode --test-frames 3 --project "${_missing_project}"
        --open-scene "Scenes/DefinitelyMissing.sparkscene" --smoke-result "${_missing_result_file}"
    WORKING_DIRECTORY "${_missing_work_dir}"
    TIMEOUT 30
    RESULT_VARIABLE _missing_result)
if(_missing_result EQUAL 0 OR NOT EXISTS "${_missing_result_file}")
    message(FATAL_ERROR "SparkEditor treated an explicit missing scene as successful (exit ${_missing_result})")
endif()
file(READ "${_missing_result_file}" _missing_json)
foreach(_marker IN ITEMS
        "\"status\": \"scene-open-failed\""
        "\"projectLoaded\": true"
        "\"runResult\": -1")
    string(FIND "${_missing_json}" "${_marker}" _marker_offset)
    if(_marker_offset EQUAL -1)
        message(FATAL_ERROR "SparkEditor missing-scene result lacks '${_marker}': ${_missing_json}")
    endif()
endforeach()

message(STATUS "SparkEditor explicit scene open passed (saved scene reopens; missing scene fails closed)")
