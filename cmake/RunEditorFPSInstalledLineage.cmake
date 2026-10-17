# Separate actual installed FPS author/cook/package lineage. No autoplay or preview substitution.
# Three install phases (120s each), family420s, validator60s and two source checks20s
# stay below the dedicated CTest900s outer bound. Artifacts remain in owned temp.
cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS
        SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TESTS_EXECUTABLE
        SPARK_ENGINE_EXECUTABLE_NAME SPARK_EXPECTED_SOURCE_SHA SPARK_PYTHON_EXECUTABLE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the editor author-to-installed-runtime test")
    endif()
endforeach()
if(NOT EXISTS "${SPARK_ENGINE_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "SPARK_ENGINE_BUILD_DIR is not a configured build tree: ${SPARK_ENGINE_BUILD_DIR}")
endif()
if(NOT EXISTS "${SPARK_TESTS_EXECUTABLE}")
    message(FATAL_ERROR "SparkTests is not built: ${SPARK_TESTS_EXECUTABLE}")
endif()

# True when _path is _root or lies below it (both normalized, case-insensitive on Windows).
function(_spark_path_is_under _path _root _out)
    file(TO_CMAKE_PATH "${_path}" _p)
    file(TO_CMAKE_PATH "${_root}" _r)
    string(REGEX REPLACE "/+$" "" _p "${_p}")
    string(REGEX REPLACE "/+$" "" _r "${_r}")
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${_p}" _p)
        string(TOLOWER "${_r}" _r)
    endif()
    string(FIND "${_p}/" "${_r}/" _position)
    if(_position EQUAL 0)
        set(${_out} TRUE PARENT_SCOPE)
    else()
        set(${_out} FALSE PARENT_SCOPE)
    endif()
endfunction()

find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SPARK_SOURCE_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE _source_sha OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _git_result TIMEOUT 10)
if(NOT "${_git_result}" STREQUAL "0" OR NOT _source_sha STREQUAL SPARK_EXPECTED_SOURCE_SHA)
    message(FATAL_ERROR "Source HEAD differs from configured lineage source")
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SPARK_SOURCE_ROOT}" diff --quiet HEAD --
    RESULT_VARIABLE _dirty_result TIMEOUT 10)
if(NOT "${_dirty_result}" STREQUAL "0")
    message(FATAL_ERROR "Installed FPS lineage requires unchanged tracked source")
endif()

# --- Test root: owned scratch space outside the source and build trees -------
if(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
    set(_temp "$ENV{TEMP}")
elseif(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(_temp "$ENV{TMPDIR}")
else()
    message(FATAL_ERROR "Neither TEMP nor TMPDIR is set; cannot place the installed-runtime test root")
endif()
file(REAL_PATH "${SPARK_ENGINE_BUILD_DIR}" _build_real)
string(MD5 _build_hash "${_build_real}")
string(SUBSTRING "${_build_hash}" 0 12 _build_hash)
file(TO_CMAKE_PATH "${_temp}/spark-editor-fps-installed-lineage/${_build_hash}/${SPARK_CONFIG}" _test_root)
get_filename_component(_test_root_parent "${_test_root}" DIRECTORY)
if(_test_root_parent STREQUAL "" OR _test_root_parent STREQUAL _test_root)
    message(FATAL_ERROR "Refusing a filesystem root as the installed-runtime test root: ${_test_root}")
endif()
foreach(_tree IN ITEMS "${SPARK_SOURCE_ROOT}" "${_build_real}")
    _spark_path_is_under("${_test_root}" "${_tree}" _inside_tree)
    if(_inside_tree)
        message(FATAL_ERROR
            "The installed-runtime test root ${_test_root} lies inside ${_tree}; the installed host "
            "must not sit below a source or build tree. Point TEMP elsewhere.")
    endif()
endforeach()

set(_marker "${_test_root}/.spark-editor-fps-installed-lineage")
if(EXISTS "${_test_root}")
    if(NOT IS_DIRECTORY "${_test_root}" OR NOT EXISTS "${_marker}")
        message(FATAL_ERROR
            "Refusing to erase ${_test_root}: it is not a test root this script created. "
            "Delete it yourself if it is scratch space.")
    endif()
    file(REMOVE_RECURSE "${_test_root}")
endif()
file(MAKE_DIRECTORY "${_test_root}/tmp" "${_test_root}/localappdata" "${_test_root}/appdata")
file(WRITE "${_marker}"
    "Scratch root staged by cmake/RunEditorFPSInstalledLineage.cmake. Safe to delete.\n")
set(_prefix "${_test_root}/prefix")

# Install the real host, FPS sample and runtime dependencies into one owned prefix.
foreach(_component IN ITEMS runtime samples redist)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}" --config "${SPARK_CONFIG}"
                --component "${_component}" --prefix "${_prefix}"
        RESULT_VARIABLE _install_result OUTPUT_VARIABLE _install_output ERROR_VARIABLE _install_error
        TIMEOUT 120)
    file(WRITE "${_test_root}/install-${_component}.log" "${_install_output}\n${_install_error}")
    if(NOT "${_install_result}" STREQUAL "0")
        message(FATAL_ERROR "Installing ${_component} failed (${_install_result})")
    endif()
endforeach()

# --- 2. The installed runtime layout the packager consumes ------------------
set(_host "${_prefix}/bin/${SPARK_ENGINE_EXECUTABLE_NAME}")
set(_module "${_prefix}/bin/SparkGameFPS.dll")
set(_evidence "${_test_root}/evidence")
file(MAKE_DIRECTORY "${_evidence}")
foreach(_expected IN ITEMS "${_host}" "${_module}" "${_module}.sparkabi"
        "${_prefix}/bin/Shaders" "${_prefix}/bin/Resources" "${_prefix}/bin/Assets")
    if(NOT EXISTS "${_expected}")
        message(FATAL_ERROR "The runtime install did not produce ${_expected}")
    endif()
endforeach()

# --- 3. Author, cook, package and run against the installed host ------------
if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "$ENV{SystemRoot}/System32" _system32)
    file(TO_NATIVE_PATH "$ENV{SystemRoot}" _system_root)
    set(_path "${_system32};${_system_root}")
else()
    set(_path "/usr/bin:/bin")
endif()
file(TO_NATIVE_PATH "${_test_root}/tmp" _tmp_native)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=SPARK_TEST_FILE --unset=SPARK_TEST_NAME --unset=SPARK_TEST_EXCLUDE --unset=SPARK_TEST_LIMIT
        "PATH=${_path}"
        "TEMP=${_tmp_native}"
        "TMP=${_tmp_native}"
        "LOCALAPPDATA=${_test_root}/localappdata"
        "APPDATA=${_test_root}/appdata"
        "SPARK_TEST_NAME_PREFIX=EditorFPSLineage_"
        "SPARK_TEST_EXPECT_COUNT=4"
        "SPARK_ENGINE_EXECUTABLE=${_host}"
        "SPARK_ENGINE_INSTALLED_PREFIX=${_prefix}"
        "SPARK_FPS_MODULE=${_module}"
        "SPARK_EDITOR_FPS_EVIDENCE_ROOT=${_evidence}"
        "SPARK_RHI_BACKEND=d3d11"
        "SPARK_D3D11_DRIVER=warp"
        "${SPARK_TESTS_EXECUTABLE}" --empty-is-error --warn-is-error
            --junit-xml "${_test_root}/lineage-junit.xml" --output-file "${_test_root}/lineage-output.log"
    WORKING_DIRECTORY "${_test_root}"
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_output
    ERROR_VARIABLE _run_error
    TIMEOUT 420)
file(WRITE "${_test_root}/sparktests.log" "${_run_output}\n${_run_error}")
message("${_run_output}\n${_run_error}")
if(NOT "${_run_result}" STREQUAL "0")
    message(FATAL_ERROR "EditorFPSLineage_ against the installed runtime failed (${_run_result}); "
                        "log: ${_test_root}/sparktests.log")
endif()

include("${SPARK_SOURCE_ROOT}/cmake/ValidateEditorFPSLineageRuntime.cmake")
spark_validate_editor_fps_lineage_runtime("${SPARK_SOURCE_ROOT}" "${_evidence}")
execute_process(
    COMMAND "${SPARK_PYTHON_EXECUTABLE}" -B
        "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/ValidateEditorFPSLineage.py"
        --evidence-root "${_evidence}" --installed-host "${_host}" --installed-module "${_module}"
        --source-sha "${_source_sha}" --config "${SPARK_CONFIG}"
        --framework-junit "${_test_root}/lineage-junit.xml"
    RESULT_VARIABLE _validation_result OUTPUT_VARIABLE _validation_output ERROR_VARIABLE _validation_error
    TIMEOUT 60)
file(WRITE "${_test_root}/lineage-validation.json" "${_validation_output}")
file(WRITE "${_test_root}/lineage-validation-error.log" "${_validation_error}")
if(NOT "${_validation_result}" STREQUAL "0")
    message(FATAL_ERROR "Installed FPS payload lineage validation failed: ${_validation_result}\n${_validation_error}")
endif()
message(STATUS "Installed FPS authored lineage passed with full evidence at ${_test_root}")
