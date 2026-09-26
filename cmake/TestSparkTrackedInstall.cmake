cmake_minimum_required(VERSION 3.25)

foreach(_spark_required IN ITEMS
        SPARK_TEST_ROOT
        SPARK_BINARY_ROOT
        SPARK_TRACKED_INSTALL_HELPER
        SPARK_UNINSTALL_HELPER)
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
if(NOT EXISTS "${SPARK_UNINSTALL_HELPER}")
    message(FATAL_ERROR "SPARK_UNINSTALL_HELPER does not exist: ${SPARK_UNINSTALL_HELPER}")
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

# Install/uninstall cycle (ASSET-220): installing twice is idempotent, the
# manifest-driven uninstall removes exactly what was installed, and afterwards
# the prefix holds only the declared user data that was written after install.
# Adversarial manifests (escapes, links, directories, semicolons, a root prefix)
# must be refused before anything is removed.
function(_spark_tree_listing root out_var)
    file(GLOB_RECURSE _spark_entries LIST_DIRECTORIES true RELATIVE "${root}" "${root}/*")
    list(SORT _spark_entries)
    set(${out_var} "${_spark_entries}" PARENT_SCOPE)
endfunction()

function(_spark_tree_digest root out_var)
    file(GLOB_RECURSE _spark_files LIST_DIRECTORIES false RELATIVE "${root}" "${root}/*")
    list(SORT _spark_files)
    set(_spark_digest "")
    foreach(_spark_file IN LISTS _spark_files)
        if(IS_SYMLINK "${root}/${_spark_file}")
            list(APPEND _spark_digest "${_spark_file}=link")
        else()
            file(SHA256 "${root}/${_spark_file}" _spark_hash)
            list(APPEND _spark_digest "${_spark_file}=${_spark_hash}")
        endif()
    endforeach()
    set(${out_var} "${_spark_digest}" PARENT_SCOPE)
endfunction()

function(_spark_run_uninstall prefix manifest out_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPREFIX=${prefix}"
            "-DMANIFEST=${manifest}"
            -P "${SPARK_UNINSTALL_HELPER}"
        RESULT_VARIABLE _spark_result
        OUTPUT_VARIABLE _spark_output
        ERROR_VARIABLE _spark_error)
    set(${out_var} "${_spark_result}" PARENT_SCOPE)
    set(${out_var}_LOG "${_spark_output}\n${_spark_error}" PARENT_SCOPE)
endfunction()

set(_spark_cycle_source "${_spark_test_root}/cycle-source")
set(_spark_cycle_build "${_spark_test_root}/cycle-build")
set(_spark_cycle_install "${_spark_test_root}/cycle-install")
set(_spark_cycle_manifest "${_spark_cycle_build}/install_manifest_runtime.txt")
set(_spark_cycle_outside "${_spark_test_root}/cycle-outside")
set(_spark_cycle_sentinel "${_spark_cycle_outside}/sentinel.txt")
file(MAKE_DIRECTORY
    "${_spark_cycle_source}/Content/a/deep"
    "${_spark_cycle_source}/Content/b"
    "${_spark_cycle_outside}")
file(WRITE "${_spark_cycle_source}/Content/root.txt" "root\n")
file(WRITE "${_spark_cycle_source}/Content/a/one.txt" "one\n")
file(WRITE "${_spark_cycle_source}/Content/a/deep/two.txt" "two\n")
file(WRITE "${_spark_cycle_source}/Content/b/three.txt" "three\n")
file(WRITE "${_spark_cycle_sentinel}" "must survive every uninstall\n")
_spark_write_fixture_project(
    "${_spark_cycle_source}" Content share/cycle runtime "\\.tmp$")
_spark_run_checked("install-cycle fixture configure"
    "${CMAKE_COMMAND}"
        -S "${_spark_cycle_source}"
        -B "${_spark_cycle_build}"
        "-DSPARK_HELPER=${SPARK_TRACKED_INSTALL_HELPER}"
        "-DCMAKE_INSTALL_PREFIX=${_spark_cycle_install}")

# User data the product writes under its prefix after installation: one file
# inside an installed directory and one in a directory the install never owned.
set(_spark_declared_user_data
    share/cycle/a/user-settings.ini
    var/Saves/slot0.sav)
set(_spark_declared_user_tree
    share
    share/cycle
    share/cycle/a
    share/cycle/a/user-settings.ini
    var
    var/Saves
    var/Saves/slot0.sav)
list(SORT _spark_declared_user_tree)

foreach(_spark_cycle IN ITEMS 1 2)
    _spark_run_checked("install-cycle ${_spark_cycle} first install"
        "${CMAKE_COMMAND}" --install "${_spark_cycle_build}" --component runtime)
    _spark_tree_listing("${_spark_cycle_install}" _spark_first_listing)
    _spark_tree_digest("${_spark_cycle_install}" _spark_first_digest)
    file(READ "${_spark_cycle_manifest}" _spark_first_manifest)
    foreach(_spark_expected IN ITEMS root.txt a/one.txt a/deep/two.txt b/three.txt)
        if(NOT EXISTS "${_spark_cycle_install}/share/cycle/${_spark_expected}")
            message(FATAL_ERROR "Install cycle ${_spark_cycle} did not install ${_spark_expected}")
        endif()
    endforeach()

    _spark_run_checked("install-cycle ${_spark_cycle} repeated install"
        "${CMAKE_COMMAND}" --install "${_spark_cycle_build}" --component runtime)
    _spark_tree_listing("${_spark_cycle_install}" _spark_second_listing)
    _spark_tree_digest("${_spark_cycle_install}" _spark_second_digest)
    file(READ "${_spark_cycle_manifest}" _spark_second_manifest)
    if(NOT _spark_second_listing STREQUAL _spark_first_listing
       OR NOT _spark_second_digest STREQUAL _spark_first_digest
       OR NOT _spark_second_manifest STREQUAL _spark_first_manifest)
        message(FATAL_ERROR
            "Repeated install in cycle ${_spark_cycle} was not idempotent\n"
            "first:  ${_spark_first_digest}\nsecond: ${_spark_second_digest}")
    endif()

    foreach(_spark_user_file IN LISTS _spark_declared_user_data)
        get_filename_component(_spark_user_dir "${_spark_cycle_install}/${_spark_user_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_spark_user_dir}")
        file(WRITE "${_spark_cycle_install}/${_spark_user_file}" "user data ${_spark_cycle}\n")
    endforeach()
    _spark_tree_digest("${_spark_cycle_install}" _spark_populated_digest)

    if(_spark_cycle EQUAL 1)
        # Every rejected manifest also lists a legitimate installed file first,
        # proving validation completes before any removal.
        set(_spark_legit_entry "${_spark_cycle_install}/share/cycle/root.txt")
        set(_spark_bad_manifest "${_spark_test_root}/cycle-bad-manifest.txt")
        set(_spark_adversarial_cases
            "outside|${_spark_cycle_sentinel}|outside the install prefix"
            "dotdot|${_spark_cycle_install}/share/../../cycle-outside/sentinel.txt|not a normalized path"
            "relative|share/cycle/root.txt|not absolute"
            "directory|${_spark_cycle_install}/share/cycle/a|is a directory"
            "reparse-parent|${_spark_cycle_install}/escape/sentinel.txt|resolves outside the install prefix")
        if(UNIX)
            list(APPEND _spark_adversarial_cases
                "file-link|${_spark_cycle_install}/share/cycle/link.txt|refusing to remove a link")
            _spark_run_checked("file link fixture"
                "${CMAKE_COMMAND}" -E create_symlink
                    "${_spark_cycle_sentinel}" "${_spark_cycle_install}/share/cycle/link.txt")
        endif()
        _spark_create_directory_reparse("${_spark_cycle_install}/escape" "${_spark_cycle_outside}")
        _spark_tree_digest("${_spark_cycle_install}" _spark_adversarial_digest)

        foreach(_spark_case IN LISTS _spark_adversarial_cases)
            string(REPLACE "|" ";" _spark_case_fields "${_spark_case}")
            list(GET _spark_case_fields 0 _spark_case_name)
            list(GET _spark_case_fields 1 _spark_case_entry)
            list(GET _spark_case_fields 2 _spark_case_reason)
            file(WRITE "${_spark_bad_manifest}" "${_spark_legit_entry}\n${_spark_case_entry}\n")
            _spark_run_uninstall("${_spark_cycle_install}" "${_spark_bad_manifest}" _spark_case_result)
            if(_spark_case_result EQUAL 0)
                message(FATAL_ERROR "Uninstall accepted the ${_spark_case_name} manifest entry")
            endif()
            if(NOT _spark_case_result_LOG MATCHES "${_spark_case_reason}")
                message(FATAL_ERROR
                    "Uninstall rejected the ${_spark_case_name} entry for an unexpected reason:\n"
                    "${_spark_case_result_LOG}")
            endif()
            _spark_tree_digest("${_spark_cycle_install}" _spark_after_case_digest)
            if(NOT _spark_after_case_digest STREQUAL _spark_adversarial_digest
               OR NOT EXISTS "${_spark_cycle_sentinel}")
                message(FATAL_ERROR
                    "Rejected ${_spark_case_name} manifest still modified the filesystem")
            endif()
        endforeach()

        file(WRITE "${_spark_bad_manifest}" "${_spark_legit_entry}\n${_spark_cycle_install}/a\;b.txt\n")
        _spark_run_uninstall("${_spark_cycle_install}" "${_spark_bad_manifest}" _spark_semicolon_result)
        if(_spark_semicolon_result EQUAL 0 OR NOT _spark_semicolon_result_LOG MATCHES "semicolon")
            message(FATAL_ERROR "Uninstall accepted a semicolon manifest:\n${_spark_semicolon_result_LOG}")
        endif()
        file(WRITE "${_spark_bad_manifest}" "${_spark_legit_entry}\n${_spark_cycle_install}/a[b.txt\n")
        _spark_run_uninstall("${_spark_cycle_install}" "${_spark_bad_manifest}" _spark_bracket_result)
        if(_spark_bracket_result EQUAL 0 OR NOT _spark_bracket_result_LOG MATCHES "unbalanced square brackets")
            message(FATAL_ERROR "Uninstall accepted an unbalanced-bracket manifest:\n${_spark_bracket_result_LOG}")
        endif()
        cmake_path(GET _spark_cycle_install ROOT_PATH _spark_filesystem_root)
        _spark_run_uninstall("${_spark_filesystem_root}" "${_spark_cycle_manifest}" _spark_root_result)
        if(_spark_root_result EQUAL 0 OR NOT _spark_root_result_LOG MATCHES "filesystem root")
            message(FATAL_ERROR "Uninstall accepted a filesystem-root prefix:\n${_spark_root_result_LOG}")
        endif()
        _spark_tree_digest("${_spark_cycle_install}" _spark_after_case_digest)
        if(NOT _spark_after_case_digest STREQUAL _spark_adversarial_digest)
            message(FATAL_ERROR "Rejected semicolon, bracket or root-prefix uninstall modified the prefix")
        endif()

        _spark_remove_directory_reparse("${_spark_cycle_install}/escape")
        if(UNIX)
            file(REMOVE "${_spark_cycle_install}/share/cycle/link.txt")
        endif()
        _spark_tree_digest("${_spark_cycle_install}" _spark_after_case_digest)
        if(NOT _spark_after_case_digest STREQUAL _spark_populated_digest)
            message(FATAL_ERROR "Adversarial fixtures were not cleaned up")
        endif()
    endif()

    _spark_run_uninstall("${_spark_cycle_install}" "${_spark_cycle_manifest}" _spark_uninstall_result)
    if(NOT _spark_uninstall_result EQUAL 0
       OR NOT _spark_uninstall_result_LOG MATCHES "removed 4 file")
        message(FATAL_ERROR
            "Uninstall cycle ${_spark_cycle} failed (${_spark_uninstall_result}):\n"
            "${_spark_uninstall_result_LOG}")
    endif()
    _spark_tree_listing("${_spark_cycle_install}" _spark_remaining)
    if(NOT _spark_remaining STREQUAL _spark_declared_user_tree)
        message(FATAL_ERROR
            "Uninstall cycle ${_spark_cycle} left undeclared content or removed user data\n"
            "expected: ${_spark_declared_user_tree}\nactual:   ${_spark_remaining}")
    endif()
    foreach(_spark_user_file IN LISTS _spark_declared_user_data)
        file(READ "${_spark_cycle_install}/${_spark_user_file}" _spark_user_content)
        if(NOT _spark_user_content STREQUAL "user data ${_spark_cycle}\n")
            message(FATAL_ERROR "Uninstall altered declared user data ${_spark_user_file}")
        endif()
    endforeach()
    if(NOT EXISTS "${_spark_cycle_sentinel}")
        message(FATAL_ERROR "Uninstall removed a file outside the prefix")
    endif()

    _spark_run_uninstall("${_spark_cycle_install}" "${_spark_cycle_manifest}" _spark_repeat_result)
    if(NOT _spark_repeat_result EQUAL 0
       OR NOT _spark_repeat_result_LOG MATCHES "removed 0 file\\(s\\), skipped 4 already absent")
        message(FATAL_ERROR
            "Repeated uninstall in cycle ${_spark_cycle} was not idempotent:\n"
            "${_spark_repeat_result_LOG}")
    endif()
endforeach()

# Glob metacharacters in the prefix: the empty-directory prune must list such a
# directory literally, so user data inside an installed directory survives.
# Windows forbids "*" and "?" in file names, so only brackets are used there.
if(WIN32)
    set(_spark_meta_install "${_spark_test_root}/cycle meta [x]/install")
else()
    set(_spark_meta_install "${_spark_test_root}/cycle meta [x]*?/install")
endif()
_spark_run_checked("glob-metacharacter prefix install"
    "${CMAKE_COMMAND}" --install "${_spark_cycle_build}" --component runtime
        --prefix "${_spark_meta_install}")
if(NOT EXISTS "${_spark_meta_install}/share/cycle/a/one.txt")
    message(FATAL_ERROR "Glob-metacharacter prefix install did not install share/cycle/a/one.txt")
endif()
foreach(_spark_user_file IN LISTS _spark_declared_user_data)
    get_filename_component(_spark_user_dir "${_spark_meta_install}/${_spark_user_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${_spark_user_dir}")
    file(WRITE "${_spark_meta_install}/${_spark_user_file}" "user data meta\n")
endforeach()
_spark_run_uninstall("${_spark_meta_install}" "${_spark_cycle_manifest}" _spark_meta_result)
if(NOT _spark_meta_result EQUAL 0 OR NOT _spark_meta_result_LOG MATCHES "removed 4 file")
    message(FATAL_ERROR
        "Glob-metacharacter prefix uninstall failed (${_spark_meta_result}):\n${_spark_meta_result_LOG}")
endif()
foreach(_spark_user_file IN LISTS _spark_declared_user_data)
    if(NOT EXISTS "${_spark_meta_install}/${_spark_user_file}")
        message(FATAL_ERROR "Glob-metacharacter prefix uninstall removed user data ${_spark_user_file}")
    endif()
    file(READ "${_spark_meta_install}/${_spark_user_file}" _spark_user_content)
    if(NOT _spark_user_content STREQUAL "user data meta\n")
        message(FATAL_ERROR "Glob-metacharacter prefix uninstall altered user data ${_spark_user_file}")
    endif()
endforeach()
foreach(_spark_pruned IN ITEMS share/cycle/a/deep share/cycle/b share/cycle/root.txt)
    if(EXISTS "${_spark_meta_install}/${_spark_pruned}")
        message(FATAL_ERROR "Glob-metacharacter prefix uninstall left ${_spark_pruned}")
    endif()
endforeach()

# DESTDIR staging: the manifest records paths without DESTDIR, and the uninstall
# helper applies the same DESTDIR (dropping a leading drive letter, as
# `cmake --install` does) so a staged install is removed in place.
set(_spark_destdir "${_spark_test_root}/cycle-destdir")
file(REMOVE_RECURSE "${_spark_cycle_install}")
_spark_run_checked("DESTDIR install"
    "${CMAKE_COMMAND}" -E env "DESTDIR=${_spark_destdir}"
        "${CMAKE_COMMAND}" --install "${_spark_cycle_build}" --component runtime)
string(REGEX REPLACE "^[A-Za-z]:" "" _spark_driveless_install "${_spark_cycle_install}")
set(_spark_staged_prefix "${_spark_destdir}${_spark_driveless_install}")
if(NOT EXISTS "${_spark_staged_prefix}/share/cycle/root.txt" OR EXISTS "${_spark_cycle_install}")
    message(FATAL_ERROR "DESTDIR install did not stage beneath DESTDIR")
endif()
file(STRINGS "${_spark_cycle_manifest}" _spark_staged_manifest)
foreach(_spark_staged_entry IN LISTS _spark_staged_manifest)
    cmake_path(IS_PREFIX _spark_cycle_install "${_spark_staged_entry}" NORMALIZE _spark_entry_unstaged)
    if(NOT _spark_entry_unstaged)
        message(FATAL_ERROR "DESTDIR manifest entry is not relative to the unstaged prefix: ${_spark_staged_entry}")
    endif()
endforeach()
_spark_run_checked("DESTDIR uninstall"
    "${CMAKE_COMMAND}" -E env "DESTDIR=${_spark_destdir}"
        "${CMAKE_COMMAND}"
            "-DPREFIX=${_spark_cycle_install}"
            "-DMANIFEST=${_spark_cycle_manifest}"
            -P "${SPARK_UNINSTALL_HELPER}")
_spark_tree_listing("${_spark_staged_prefix}" _spark_staged_remaining)
if(NOT _spark_staged_remaining STREQUAL "")
    message(FATAL_ERROR "DESTDIR uninstall left content: ${_spark_staged_remaining}")
endif()

message(STATUS "Spark tracked-install helper tests passed")
