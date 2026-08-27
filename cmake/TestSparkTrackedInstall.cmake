cmake_minimum_required(VERSION 3.25)

foreach(_spark_required IN ITEMS
        SPARK_TEST_ROOT
        SPARK_BINARY_ROOT
        SPARK_TRACKED_INSTALL_HELPER)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "${_spark_required} is required")
    endif()
endforeach()

set(_spark_test_root "${SPARK_TEST_ROOT}")
set(_spark_binary_root "${SPARK_BINARY_ROOT}")
cmake_path(ABSOLUTE_PATH _spark_test_root NORMALIZE)
cmake_path(ABSOLUTE_PATH _spark_binary_root NORMALIZE)
cmake_path(IS_PREFIX _spark_binary_root "${_spark_test_root}"
    NORMALIZE _spark_test_is_bounded)
cmake_path(COMPARE "${_spark_binary_root}" EQUAL "${_spark_test_root}"
    _spark_test_is_binary_root)
if(NOT _spark_test_is_bounded OR _spark_test_is_binary_root)
    message(FATAL_ERROR
        "Refusing to use tracked-install scratch path outside or equal to the build tree")
endif()
if(NOT EXISTS "${SPARK_TRACKED_INSTALL_HELPER}")
    message(FATAL_ERROR
        "SPARK_TRACKED_INSTALL_HELPER does not exist: ${SPARK_TRACKED_INSTALL_HELPER}")
endif()

file(REMOVE_RECURSE "${_spark_test_root}")
file(MAKE_DIRECTORY "${_spark_test_root}")
find_package(Git REQUIRED)

function(_spark_run_checked description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _spark_result
        OUTPUT_VARIABLE _spark_output
        ERROR_VARIABLE _spark_error)
    if(NOT _spark_result EQUAL 0)
        message(FATAL_ERROR
            "${description} failed (${_spark_result})\n${_spark_output}\n${_spark_error}")
    endif()
endfunction()

function(_spark_create_directory_reparse _spark_link _spark_target)
    if(WIN32)
        cmake_path(NATIVE_PATH _spark_link _spark_link_native)
        cmake_path(NATIVE_PATH _spark_target _spark_target_native)
        execute_process(
            COMMAND cmd /c mklink /J "${_spark_link_native}" "${_spark_target_native}"
            RESULT_VARIABLE _spark_link_result
            OUTPUT_VARIABLE _spark_link_output
            ERROR_VARIABLE _spark_link_error)
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "${_spark_target}" "${_spark_link}"
            RESULT_VARIABLE _spark_link_result
            OUTPUT_VARIABLE _spark_link_output
            ERROR_VARIABLE _spark_link_error)
    endif()
    if(NOT _spark_link_result EQUAL 0)
        message(FATAL_ERROR
            "Could not create directory reparse fixture (${_spark_link_result})\n"
            "${_spark_link_output}\n${_spark_link_error}")
    endif()
endfunction()

function(_spark_remove_directory_reparse _spark_link)
    if(WIN32)
        cmake_path(NATIVE_PATH _spark_link _spark_link_native)
        execute_process(
            COMMAND cmd /c rmdir "${_spark_link_native}"
            RESULT_VARIABLE _spark_unlink_result
            OUTPUT_VARIABLE _spark_unlink_output
            ERROR_VARIABLE _spark_unlink_error)
    else()
        file(REMOVE "${_spark_link}")
        set(_spark_unlink_result 0)
    endif()
    if(NOT _spark_unlink_result EQUAL 0)
        message(FATAL_ERROR
            "Could not remove directory reparse fixture (${_spark_unlink_result})\n"
            "${_spark_unlink_output}\n${_spark_unlink_error}")
    endif()
endfunction()

function(_spark_write_fixture_project source_dir source_path destination component exclusion)
    file(WRITE "${source_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(SparkTrackedInstallFixture NONE)\n"
        "include(\"\${SPARK_HELPER}\")\n"
        "spark_install_tracked_directory(\n"
        "    SOURCE \"${source_path}\"\n"
        "    DESTINATION \"${destination}\"\n"
        "    COMPONENT \"${component}\"\n"
        "    EXCLUDE_REGEXES \"${exclusion}\")\n")
endfunction()

# Git checkout: tracked content is installed, while an untracked neighbor and a
# tracked-but-excluded test tree are absent. The Python script also exercises
# install(PROGRAMS), which preserves executable usability.
set(_spark_git_source "${_spark_test_root}/git-source")
file(MAKE_DIRECTORY
    "${_spark_git_source}/Tools/bin"
    "${_spark_git_source}/Tools/data"
    "${_spark_git_source}/Tools/tests")
file(WRITE "${_spark_git_source}/Tools/bin/run.py" "#!/usr/bin/env python3\nprint('tracked')\n")
file(WRITE "${_spark_git_source}/Tools/data/tracked.txt" "tracked\n")
file(WRITE "${_spark_git_source}/Tools/data/local-secret.txt" "untracked\n")
file(WRITE "${_spark_git_source}/Tools/tests/tracked_test.py" "raise SystemExit(0)\n")
_spark_write_fixture_project(
    "${_spark_git_source}" Tools tools tools "/tests(/|$)")
_spark_run_checked("git init"
    "${GIT_EXECUTABLE}" -C "${_spark_git_source}" init --quiet)
_spark_run_checked("git add"
    "${GIT_EXECUTABLE}" -C "${_spark_git_source}" add --
        Tools/bin/run.py Tools/data/tracked.txt Tools/tests/tracked_test.py)

set(_spark_git_build "${_spark_test_root}/git-build")
set(_spark_git_install "${_spark_test_root}/git-install")
_spark_run_checked("tracked fixture configure"
    "${CMAKE_COMMAND}"
        -S "${_spark_git_source}"
        -B "${_spark_git_build}"
        "-DSPARK_HELPER=${SPARK_TRACKED_INSTALL_HELPER}"
        "-DCMAKE_INSTALL_PREFIX=${_spark_git_install}")
_spark_run_checked("tracked fixture install"
    "${CMAKE_COMMAND}" --install "${_spark_git_build}" --component tools)

foreach(_spark_expected IN ITEMS tools/bin/run.py tools/data/tracked.txt)
    if(NOT EXISTS "${_spark_git_install}/${_spark_expected}")
        message(FATAL_ERROR "Tracked file was not installed: ${_spark_expected}")
    endif()
endforeach()
foreach(_spark_forbidden IN ITEMS tools/data/local-secret.txt tools/tests/tracked_test.py)
    if(EXISTS "${_spark_git_install}/${_spark_forbidden}")
        message(FATAL_ERROR "Forbidden file was installed: ${_spark_forbidden}")
    endif()
endforeach()
if(UNIX)
    execute_process(
        COMMAND test -x "${_spark_git_install}/tools/bin/run.py"
        RESULT_VARIABLE _spark_script_is_not_executable)
    if(_spark_script_is_not_executable)
        message(FATAL_ERROR "Installed script is not executable")
    endif()
endif()

# The NUL-delimited Git manifest is decoded byte-by-byte specifically so a
# semicolon cannot become a CMake list separator. Confirm that such a tracked
# filename is rejected instead of being split into package entries.
file(WRITE "${_spark_git_source}/Tools/data/bad;name.txt" "malformed path\n")
_spark_run_checked("git add malformed path fixture"
    "${GIT_EXECUTABLE}" -C "${_spark_git_source}" add --all -- Tools)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_spark_git_source}"
        -B "${_spark_test_root}/malformed-path-build"
        "-DSPARK_HELPER=${SPARK_TRACKED_INSTALL_HELPER}"
        "-DCMAKE_INSTALL_PREFIX=${_spark_test_root}/malformed-path-install"
    RESULT_VARIABLE _spark_malformed_path_result
    OUTPUT_VARIABLE _spark_malformed_path_output
    ERROR_VARIABLE _spark_malformed_path_error)
if(_spark_malformed_path_result EQUAL 0)
    message(FATAL_ERROR "Tracked semicolon filename was accepted")
endif()
if(NOT "${_spark_malformed_path_output}\n${_spark_malformed_path_error}"
       MATCHES "semicolon")
    message(FATAL_ERROR
        "Tracked semicolon filename failed for an unexpected reason:\n"
        "${_spark_malformed_path_output}\n${_spark_malformed_path_error}")
endif()

# Source distribution: with no local .git metadata, explicit directory install
# includes the supplied tree while still honoring the caller's exclusion regex.
set(_spark_dist_source "${_spark_test_root}/dist-source")
file(MAKE_DIRECTORY "${_spark_dist_source}/Assets/Runtime/nested")
file(WRITE "${_spark_dist_source}/Assets/Runtime/nested/kept.dat" "archive content\n")
file(WRITE "${_spark_dist_source}/Assets/Runtime/nested/editor.tmp" "excluded\n")
_spark_write_fixture_project(
    "${_spark_dist_source}" Assets/Runtime bin/Assets/Runtime runtime "\\.tmp$")

set(_spark_dist_build "${_spark_test_root}/dist-build")
set(_spark_dist_install "${_spark_test_root}/dist-install")
_spark_run_checked("source-distribution fixture configure"
    "${CMAKE_COMMAND}"
        -S "${_spark_dist_source}"
        -B "${_spark_dist_build}"
        "-DSPARK_HELPER=${SPARK_TRACKED_INSTALL_HELPER}"
        "-DCMAKE_INSTALL_PREFIX=${_spark_dist_install}")
_spark_run_checked("source-distribution fixture install"
    "${CMAKE_COMMAND}" --install "${_spark_dist_build}" --component runtime)
if(NOT EXISTS "${_spark_dist_install}/bin/Assets/Runtime/nested/kept.dat")
    message(FATAL_ERROR "Source-distribution fallback omitted archive content")
endif()
if(EXISTS "${_spark_dist_install}/bin/Assets/Runtime/nested/editor.tmp")
    message(FATAL_ERROR "Source-distribution fallback ignored its exclusion regex")
endif()

# The install rule must consume the configure-time payload snapshot. Replacing
# a tracked file's parent with a junction/symlink after configure must never
# redirect installation to attacker-controlled live checkout content.
set(_spark_swap_source "${_spark_test_root}/parent-swap-source")
set(_spark_swap_parent "${_spark_swap_source}/Content/parent")
set(_spark_swap_saved_parent "${_spark_swap_source}/Content/verified-parent")
set(_spark_swap_attacker_parent "${_spark_test_root}/parent-swap-attacker")
file(MAKE_DIRECTORY "${_spark_swap_parent}" "${_spark_swap_attacker_parent}")
file(WRITE "${_spark_swap_parent}/payload.txt" "verified snapshot\n")
file(WRITE "${_spark_swap_attacker_parent}/payload.txt" "attacker replacement\n")
_spark_write_fixture_project(
    "${_spark_swap_source}" Content share/content runtime "\\.tmp$")
_spark_run_checked("parent-swap git init"
    "${GIT_EXECUTABLE}" -C "${_spark_swap_source}" init --quiet)
_spark_run_checked("parent-swap git add"
    "${GIT_EXECUTABLE}" -C "${_spark_swap_source}" add -- Content/parent/payload.txt)
set(_spark_swap_build "${_spark_test_root}/parent-swap-build")
set(_spark_swap_install "${_spark_test_root}/parent-swap-install")
_spark_run_checked("parent-swap fixture configure"
    "${CMAKE_COMMAND}"
        -S "${_spark_swap_source}"
        -B "${_spark_swap_build}"
        "-DSPARK_HELPER=${SPARK_TRACKED_INSTALL_HELPER}"
        "-DCMAKE_INSTALL_PREFIX=${_spark_swap_install}")
file(RENAME "${_spark_swap_parent}" "${_spark_swap_saved_parent}")
_spark_create_directory_reparse("${_spark_swap_parent}" "${_spark_swap_attacker_parent}")
_spark_run_checked("parent-swap fixture install"
    "${CMAKE_COMMAND}" --install "${_spark_swap_build}" --component runtime)
set(_spark_swap_installed "${_spark_swap_install}/share/content/parent/payload.txt")
if(NOT EXISTS "${_spark_swap_installed}")
    message(FATAL_ERROR "Parent-swap fixture installed no payload")
endif()
file(READ "${_spark_swap_installed}" _spark_swap_installed_content)
if(NOT _spark_swap_installed_content STREQUAL "verified snapshot\n")
    message(FATAL_ERROR
        "Parent-swap fixture escaped the verified configure-time snapshot")
endif()
_spark_remove_directory_reparse("${_spark_swap_parent}")

# A directory that advertises local .git metadata but is not a valid checkout
# must fail configuration. This guards against accidentally converting a Git
# command failure into the permissive source-distribution path.
set(_spark_broken_source "${_spark_test_root}/broken-git-source")
file(MAKE_DIRECTORY
    "${_spark_broken_source}/.git"
    "${_spark_broken_source}/Content")
file(WRITE "${_spark_broken_source}/Content/file.txt" "must not install\n")
_spark_write_fixture_project(
    "${_spark_broken_source}" Content content runtime "\\.tmp$")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_spark_broken_source}"
        -B "${_spark_test_root}/broken-git-build"
        "-DSPARK_HELPER=${SPARK_TRACKED_INSTALL_HELPER}"
        "-DCMAKE_INSTALL_PREFIX=${_spark_test_root}/broken-git-install"
    RESULT_VARIABLE _spark_broken_git_result
    OUTPUT_VARIABLE _spark_broken_git_output
    ERROR_VARIABLE _spark_broken_git_error)
if(_spark_broken_git_result EQUAL 0)
    message(FATAL_ERROR
        "Invalid local .git metadata silently selected the source-distribution fallback")
endif()
if(NOT "${_spark_broken_git_output}\n${_spark_broken_git_error}"
       MATCHES "(refusing fallback|different repository root|outside CMAKE_SOURCE_DIR)")
    message(FATAL_ERROR
        "Invalid local .git metadata failed for an unexpected reason:\n"
        "${_spark_broken_git_output}\n${_spark_broken_git_error}")
endif()

message(STATUS "Spark tracked-install helper tests passed")
