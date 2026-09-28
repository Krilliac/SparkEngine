# EDT-210 author-to-installed-runtime lane, run as a CTest via `cmake -P`.
#
# EditorCookPackageRoundTrip authors a scene through the production editor
# paths, cooks it with SparkCooker, packages it with AssembleNativePackage and
# runs the packaged scene host, but the host it packages is the build-tree
# SparkEngine. This lane runs the same two EditorCookPackage_ tests against an
# INSTALLED runtime:
#
#   1. `cmake --install <build> --component runtime` into a fresh prefix that
#      this script owns, outside the source and build trees;
#   2. require <prefix>/bin/<engine>, bin/Shaders and bin/Resources;
#   3. run SparkTests with a minimal environment (PATH = the Windows system
#      directories, TEMP/TMP under the test root, working directory = the test
#      root) and SPARK_ENGINE_EXECUTABLE / SPARK_ENGINE_INSTALLED_PREFIX naming
#      the installed host. The tests then assert that the host lies under the
#      prefix and that no CMakeCache.txt sits above it, so a build-tree host
#      cannot satisfy this lane.
#
# The test root is <temp>/spark-edt210-installed-runtime/<build-dir hash>/<config>.
# It is erased and recreated on every run, and only when it carries this
# script's ownership marker (or does not exist yet).
#
# Required: SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG
#           SPARK_TESTS_EXECUTABLE SPARK_ENGINE_EXECUTABLE_NAME
#           SPARK_STAGE_TIMEOUT (seconds per stage; the caller keeps
#           2 x SPARK_STAGE_TIMEOUT below the CTest TIMEOUT)

cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS
        SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TESTS_EXECUTABLE
        SPARK_ENGINE_EXECUTABLE_NAME SPARK_STAGE_TIMEOUT)
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
file(TO_CMAKE_PATH "${_temp}/spark-edt210-installed-runtime/${_build_hash}/${SPARK_CONFIG}" _test_root)
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

set(_marker "${_test_root}/.spark-editor-installed-runtime")
if(EXISTS "${_test_root}")
    if(NOT IS_DIRECTORY "${_test_root}" OR NOT EXISTS "${_marker}")
        message(FATAL_ERROR
            "Refusing to erase ${_test_root}: it is not a test root this script created. "
            "Delete it yourself if it is scratch space.")
    endif()
    file(REMOVE_RECURSE "${_test_root}")
endif()
file(MAKE_DIRECTORY "${_test_root}/tmp")
file(WRITE "${_marker}"
    "Scratch root staged by cmake/RunEditorAuthorInstalledRuntime.cmake. Safe to delete.\n")
set(_prefix "${_test_root}/prefix")

# --- 1. Install the runtime component --------------------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}" --config "${SPARK_CONFIG}"
            --component runtime --prefix "${_prefix}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error
    TIMEOUT ${SPARK_STAGE_TIMEOUT})
file(WRITE "${_test_root}/install.log" "${_install_output}\n${_install_error}")
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "Installing the runtime component failed (${_install_result}):\n"
                        "${_install_output}\n${_install_error}")
endif()

# --- 2. The installed runtime layout the packager consumes ------------------
set(_host "${_prefix}/bin/${SPARK_ENGINE_EXECUTABLE_NAME}")
foreach(_expected IN ITEMS "${_host}" "${_prefix}/bin/Shaders" "${_prefix}/bin/Resources")
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
        "SPARK_TEST_NAME_PREFIX=EditorCookPackage_"
        "SPARK_TEST_EXPECT_COUNT=2"
        "SPARK_ENGINE_EXECUTABLE=${_host}"
        "SPARK_ENGINE_INSTALLED_PREFIX=${_prefix}"
        "${SPARK_TESTS_EXECUTABLE}" --empty-is-error
    WORKING_DIRECTORY "${_test_root}"
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_output
    ERROR_VARIABLE _run_error
    TIMEOUT ${SPARK_STAGE_TIMEOUT})
file(WRITE "${_test_root}/sparktests.log" "${_run_output}\n${_run_error}")
message("${_run_output}\n${_run_error}")
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "EditorCookPackage_ against the installed runtime failed (${_run_result}); "
                        "log: ${_test_root}/sparktests.log")
endif()
message(STATUS "EditorCookPackage_ passed against ${_host}")
